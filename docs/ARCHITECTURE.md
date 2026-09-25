# Architecture and latency design

## Where the milliseconds actually go

For a news-driven signal, end-to-end latency is:

```
source publishes ──(A) source's own delay──> item visible at the endpoint
                 ──(B) our poll cadence────> we request it
                 ──(C) network round trip──> bytes arrive
                 ──(D) our processing──────> signal emitted
```

* **(A)** depends on the source. EDGAR's "latest filings" feed updates as filings
  are accepted. Public wire RSS can trail the wire's own paid machine-readable
  feeds. You can't optimize (A), only pick the sources where it is smallest,
  so the daemon measures it (see *Choosing the fastest sources*).
* **(B)** Staggered lanes: a feed with `interval_ms = 500, lanes = 2` has two
  warm connections, each polling every 1 s, offset by 500 ms. Effective cadence is 500 ms,
  and a slow response on one lane never delays the next poll. Per-host token
  buckets keep the total within each publisher's fair-access limits (SEC: 10 req/s).
* **(C)** One round trip per poll, because connections stay warm: no DNS, TCP
  or TLS handshake on the hot path. `TCP_NODELAY`, `TCP_QUICKACK`, TLS session
  resumption, gzip, and conditional GETs (an unchanged feed costs a ~200-byte 304).
  Idle connections the server closes are reopened immediately. Connections
  nearing the server's advertised `Keep-Alive: timeout` are refreshed early, so
  a poll never lands on a dying socket. Deploy near the sources (US-East).
* **(D)** Microseconds; see the table in the README. It covers zero-copy parsing,
  single-pass scoring, lock-free hand-offs and no allocation on the hot path.

(A) and (C) are tens of milliseconds, (D) is tens of microseconds. That's why
most of the engineering effort sits in the network layer, and (D) is kept fast
enough to never matter.

## Threads and data flow

```
            ┌──────────── net thread (pinned) ─────────────┐
 sockets ──>│ epoll_pwait2 → Connection::on_events         │
            │ → ResponseParser (incremental) → Inflater    │
            │ → scan_feed: hash ids, skip seen             │
            │ → fill_event directly into the ring slot     │── SpscQueue<NewsEvent> ──┐
            │ → exhibit jobs (8-K/6-K → index → EX-99)     │                          │
            └──────────────────────────────────────────────┘                          v
            ┌──────────── market thread (pinned) ──────────┐              ┌──── engine thread (pinned) ────┐
 multicast─>│ recvmmsg batch → MoldUDP64 seq check         │  seqlock     │ on_news: score, universe,      │
 or file  ─>│ → itch::decode (switch, bswap)               │─ per symbol ─│ dilution, watchlist, emit      │
 or bridge  │ → order map (open addressing) → MarketBoard  │  reads       │ evaluate(): confirmation tiers │
            │ → halts/system events ───────────────────────│─ SpscQueue ──│ scan_movers(), halts, outcomes │
            └──────────────────────────────────────────────┘ <MarketEvent>└────┬──────────────────┬────────┘
                                                                              │ SpscQueue<Signal> │ SpscQueue<JournalRecord>
                                                                              v                   v
                                                                     alerts thread          journal thread
                                                                 (stdout, Telegram over   (JSON formatting and
                                                                  a warm connection, UDP)  file I/O off the hot path)
```

Rules that keep it fast:

* **One owner per piece of state.** No mutex anywhere on the hot path. Threads
  communicate only through single-producer/single-consumer rings (cache-line
  separated indices, cached opposite index, write-in-place slots) and per-symbol
  seqlocks (the market thread writes; the engine reads consistent snapshots without blocking it).
* **No allocation in steady state.** Receive buffers, inflate output, parse
  scratch space and event slots are reused. Events are fixed-size, trivially copyable structs.
* **Two-pass feed parsing.** Pass 1 hashes each item's guid/id. Only unseen
  items are parsed fully (entity decoding, HTML stripping, ticker extraction).
* **One pass to score.** All rules are compiled into one Aho-Corasick DFA over a
  37-symbol alphabet. Separator folding gives word boundaries for free.
* **Cheap wake-ups.** Consumers sleep on an eventfd only when their queues are
  empty. Producers make the `write()` syscall only if the consumer is actually
  asleep (Dekker-style fences prevent lost wake-ups). With `busy_poll = true` the
  consumers spin instead, which gives sub-microsecond hand-offs on isolated cores.
* **Formatting and I/O last.** The engine hands structs to the journal and alert
  threads, which do the JSON and the syscalls.

## Choosing the fastest sources

Public, free sources are not the fastest possible. In rough order of speed:

1. **Paid direct feeds**: the wires' machine-readable push feeds, and the SEC's
   Public Dissemination Service (PDS) push feed of EDGAR filings. These need
   subscriptions and dedicated connectivity. The feed layer is where a TCP-push
   source kind would plug in.
2. **EDGAR latest-filings Atom and the filing index pages**: official, free,
   close to acceptance time. Many small caps file the 8-K with the press release
   as EX-99.1 around the time the wire story goes out, and sometimes before.
   That's why exhibits are fetched automatically.
3. **Wire RSS**: free. Timing varies by wire and by time of day.

Don't guess, measure:

* `stokd --probe` reports cold and warm latency, 304 support and staleness for each feed.
* The engine keeps a cross-source first-seen table keyed by a normalized headline
  hash. The periodic `engine stats` log shows, per source, how often it was
  **first**, how often it was **behind**, and by how many milliseconds.
* `news.jsonl` records `recv_ns`, `sent_ns`, `pub_ns` and `behind_first_ms` for
  every item. After a week, drop or deprioritize the sources that are never first.

## Market data

* **ITCH 5.0** decoding follows the Nasdaq spec: fixed offsets, big-endian
  fields, 6-byte timestamps (ns since midnight ET). Executions (`E`, printable
  `C`), hidden executions (`P`) and crosses (`Q`) count as volume. Orders are
  tracked in an open-addressing map with backward-shift deletion, so executions
  can be priced.
* **Coverage caveat.** Nasdaq ITCH carries Nasdaq's own executions, a fraction
  of consolidated volume. RVOL is computed against a baseline built from the
  same feed (`stok-replay --baseline`, plus the daemon's end-of-day append), so
  it compares like with like. Absolute dollar-volume thresholds are relative to that coverage.
* **Top of book.** Resting orders are aggregated into per-symbol price ladders
  (sorted vectors with the best price at the back, so updates at the touch move
  almost nothing). The best bid/ask goes to the board only when it changes.
  That's the Nasdaq book, not the consolidated NBBO. It's complete only if the
  feed is read from the start of the session: joined mid-day, orders entered
  earlier are unknown. A Glimpse snapshot isn't implemented, and crossed or
  one-sided quotes are treated as unknown.
* **MoldUDP64**: `recvmmsg` batches, optional `SO_BUSY_POLL`, sequence tracking.
  With `mold_rerequest` set, a gap triggers a re-request for the missing range
  (retried every 50 ms). Newer packets are buffered and everything is replayed
  strictly in sequence, so the order book never sees messages out of order.
  After `mold_gap_timeout_ms` the receiver gives up, counts the lost messages and
  continues from the buffer.
* **Bridge**: any other source can feed trades and states over a UDP text protocol.

## Paper trading and evaluation

The paper trader runs inside the engine thread and reads the same seqlocked
quotes, so it adds no queues or locks.
* **Entry:** on ALERT/HIGH, the order "arrives" after `latency_ms` and buys at the
  ask (or the last trade if there's no quote) plus `slippage_bps`. It cancels if
  the price ran more than `max_chase_pct` past the signal price, or if the stock
  stays halted beyond `entry_timeout_s`.
* **Exit:** sells at the bid minus slippage on stop, target, trailing stop, time
  limit or flat-by time. It can't sell while the stock is halted, so a gap
  through the stop is booked at the reopening price, as it would be for real.
* **What it doesn't model:** it assumes our size fills at the touch. Trades where
  the order was larger than the displayed ask size are flagged (`size_over_touch`)
  so they can be judged separately. Queue position and hidden liquidity aren't simulated.

`stok-report` reads the journal and reports:
* per source: how often it was first and how far behind otherwise;
* the funnel from stories to signals;
* price outcomes by tier, catalyst and score bucket;
* paper-trading expectancy, profit factor, drawdown, and the result without the best three trades;
* a PASS/FAIL checklist against the go/no-go criteria in PLAN.md section 5.

## OS tuning for production

* Kernel command line: `isolcpus=2-4 nohz_full=2-4 rcu_nocbs=2-4`. Then pin
  `net_cpu`, `engine_cpu` and `market_cpu` to those cores.
* Move NIC IRQs off the isolated cores (`/proc/irq/*/smp_affinity`), or onto the
  core that owns the matching socket.
* `cpupower frequency-set -g performance`. Disable deep C-states if latency jitter matters.
* `[threads] mlock = true` and `rt_priority = 50`. The systemd unit grants
  `CAP_IPC_LOCK` and `CAP_SYS_NICE`.
* Turn on `busy_poll` in `[net]`, `[signal]` and `[market]` only on isolated cores: it burns 100% of each.

## Limitations (current)

* Feed URLs couldn't be verified from the build environment, which blocks
  outbound access to those hosts. Run `stokd --probe` on your server first.
* Top of book is Nasdaq's own book (via ITCH), and it's complete only from the
  session start (no Glimpse snapshot recovery).
* Company-name matching only runs when a story has no exchange tag. It only
  matches listed issuers, skips ambiguous names, and flags matches (`name_matched`)
  so they can be judged separately.
* Paper fills assume the displayed size at the touch is available (see above).
* The rule weights are starting guesses. Tune them against the journal
  (`news.jsonl` joined to `outcomes.jsonl`) before trusting any tier.
