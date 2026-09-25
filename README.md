# stok

A low-latency penny-stock news and signal engine in C++20. It watches official
and primary sources (SEC EDGAR, press-release wires, Nasdaq halts), scores each
story for real materiality rather than tone, confirms the move with exchange
market data, and pushes tiered alerts (stdout, Telegram, UDP JSON). Every story,
signal and outcome is written to a journal so you can measure whether an edge
exists.

> Research tool, not financial advice. See [PLAN.md](PLAN.md) for how realistic a
> profitable bot is, and why the journal and outcome tracking come before real money.

```
 SEC EDGAR ─┐                              ┌──> stdout / Telegram (warm conn) / UDP JSON
 Wires RSS ─┤  net thread     lock-free    │
 Nasdaq    ─┤  (epoll, TLS,   SPSC ring    engine thread ──> journal (JSONL: news, signals, outcomes)
 halts     ─┘   keep-alive) ─────────────> (score, universe,
                                            dilution, tiers) <── seqlocked MarketBoard
 Nasdaq ITCH 5.0 (MoldUDP64 / file) ─> market thread ─────────────────┘
 or UDP bridge from any source
```

## What it does

| Stage | How |
|---|---|
| **Collect** | Polls EDGAR, press-release wires and the Nasdaq halts feed. Each feed has several warm keep-alive TLS connections firing in staggered phases, with conditional GETs (304s) and per-host rate limits. For each 8-K/6-K it auto-fetches the EX-99 press-release exhibit from EDGAR. |
| **Parse** | Zero-copy XML scanning. Each item's id is hashed first, so only new items get parsed. Exchange-tagged tickers are extracted (`(NASDAQ: ABCD)`, `OTCQB:`, `NYSE American:`...) and resolved against the official security master. |
| **Score** | An Aho-Corasick DFA runs over about 320 tunable rules (`config/rules.tsv`) in one pass. It picks the catalyst, and checks for definitive vs. non-binding language, named top-tier counterparties, hedging and promotional language. Offerings, law-firm spam and distress are separated out. The dollar amount found is compared with market cap (materiality). |
| **Filter** | Universe checks: exchange-listed, price band, market cap, no ETFs, warrants or units. Dilution and distress flags come from SEC filings: recent 424B or EFFECT, an S-3 shelf, S-1, item 3.02, reverse-split proxies, late filers, and Nasdaq deficiency status. |
| **Confirm** | Price and volume from exchange data: move since the news, dollar volume since the news, RVOL against a self-built baseline, VWAP, and halts. |
| **Signal** | `WATCH` (material news) → `ALERT` (the stock is moving on volume) → `HIGH` (strong score, small share count, clean filings, above VWAP). Also `HALT`, `MOVER` (big mover with no qualifying news) and `INFO` (an offering filed on a watched ticker). |
| **Measure** | JSONL journal of every item, with per-source arrival times, cross-source "who had it first" stats, and price/volume at +1/5/15/30/60 minutes after every watched story. |

## Official data sources

| Data | Source | Cost |
|---|---|---|
| Filings, 8-K items, exhibits | SEC EDGAR "latest filings" Atom plus filing index pages | free |
| Security master (all US-listed), listing tier, deficiency flags | Nasdaq Trader symbol directories | free |
| Ticker ↔ CIK ↔ exchange | SEC `company_tickers_exchange.json` | free |
| Shares outstanding, public float | SEC XBRL frames API (`dei:` cover-page facts) | free |
| Dilution history (S-1/S-3/424B/EFFECT/PRE 14A/NT) | SEC EDGAR daily form indexes | free |
| Short-sale volume | FINRA Reg SHO daily files | free |
| Halts / LULD pauses | Nasdaq Trader halts RSS, and ITCH `H` messages | free / with ITCH |
| Real-time trades | Nasdaq TotalView-ITCH 5.0 over MoldUDP64, the exchange's own binary protocol | Nasdaq subscription plus connectivity |
| Press releases | Wire RSS (GlobeNewswire, PR Newswire, Business Wire; Accesswire and Newsfile configurable) | free |

No third-party data vendor or AI API is on the hot path. Real-time exchange data
is never free. If you don't have an ITCH feed, run news-only (WATCH and halts),
or feed trades from any source through the UDP bridge (`market.source = bridge`).

## Performance

Measured in the development container (4 vCPU VM) with `./build/stok-bench`.
Expect better on a tuned bare-metal box.

| Hot-path step | Time |
|---|---|
| Parse one new item → event (entities, HTML strip, tickers) | ~1.6–2 µs |
| Score a 3 KB story (320 rules, one pass) | ~11 µs (~290 MB/s) |
| Engine: story → signal + journal record | ~1.5 µs |
| Scan an unchanged 100-item feed (ids only) | ~50 µs (and usually a 304 means no scan at all) |
| ITCH decode + order book + board update | ~60 ns/msg (16M msg/s; Nasdaq peaks around 2M/s) |
| Seqlock snapshot of a symbol | ~11 ns |
| Response received → WATCH emitted (daemon, incl. thread hop) | ~0.2 ms, or sub-µs hops with `busy_poll` on pinned cores |

The network round trip to the source (5–80 ms) and the source's own publishing
delay dominate. That's why the design focuses on warm connections, staggered
polling, conditional requests and measuring which source is actually first.
See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Build

Dependencies: a C++20 compiler (GCC 12+ or Clang 15+), CMake 3.20+, OpenSSL and zlib.

```bash
sudo apt install build-essential cmake ninja-build libssl-dev zlib1g-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release   # -march=native + LTO by default
ninja -C build
ctest --test-dir build --output-on-failure                # unit tests + local TLS integration test
python3 scripts/demo.py                                   # offline end-to-end demo of stokd
```

## Run

```bash
# 1. Put your name + email in net.user_agent (SEC answers 403 without it).
$EDITOR config/stok.conf

# 2. Download official reference data (~1 minute; re-run daily, it is incremental).
./build/stok-ref -c config/stok.conf

# 3. Check every feed from your server: latency, 304 support, staleness.
./build/stokd -c config/stok.conf --probe

# 4. Run. Alerts go to stdout; set Telegram via environment variables.
export STOK_TELEGRAM_TOKEN=123:abc STOK_TELEGRAM_CHAT_ID=987654
./build/stokd -c config/stok.conf
```

A deployment unit is in `deploy/stokd.service`. Run it on a VPS in US-East:
Nasdaq, the SEC and the wires' CDNs are all nearby.

### Market data options (`[market] source =`)

* `none`: news-only. You get WATCH signals, halts from RSS, and outcome logging without prices.
* `moldudp64`: live Nasdaq TotalView-ITCH (group/port/interface in the config).
* `itch_file`: replay a Nasdaq ITCH file (`scripts/fetch_itch_sample.sh`) at any speed.
* `bridge`: UDP text lines `T SYM PRICE SIZE [EPOCH_NS]` and `H SYM STATE [REASON]` from any other source.

Build the RVOL baseline from history with
`./build/stok-replay data/itch/<file>.gz --baseline -c config/stok.conf`. The
daemon also appends each live session to it at the end of the day.

## Tools

| Binary | Purpose |
|---|---|
| `stokd` | The daemon (`--probe` checks the feeds and exits) |
| `stok-ref` | Official reference-data downloader (Nasdaq Trader, SEC, FINRA) |
| `stok-replay` | ITCH replay: throughput benchmark, most-active report, baseline builder |
| `stok-bench` | Microbenchmarks for every hot-path stage |
| `stok-tests` | Unit tests (`stok-tests <filter>` runs a subset) |

## Journal

`data/journal/YYYY-MM-DD/{news,signals,outcomes}.jsonl`. DuckDB reads it directly:

```sql
SELECT n.catalyst, count(*) AS stories, avg(o.move_pct) AS avg_5m, avg(o.max_move_pct) AS avg_mfe
FROM 'data/journal/*/news.jsonl' n
JOIN 'data/journal/*/outcomes.jsonl' o ON o.news_id = n.id AND o.horizon_s = 300
GROUP BY 1 ORDER BY stories DESC;
```

## Layout

```
src/core     lock-free SPSC/MPSC rings, seqlock, flat hash map, eventfd waker, async logger, config
src/net      non-blocking HTTP/1.1 + TLS connection state machine, parser, inflater, DNS cache, client
src/feeds    zero-copy RSS/Atom scanning, EDGAR/halts parsers, ticker extraction, feed poller
src/ref      security master (Nasdaq Trader + SEC), filings history / dilution flags
src/market   ITCH 5.0 decoder, order book, seqlocked MarketBoard, MoldUDP64/file/bridge sources, baseline
src/engine   rule scorer (Aho-Corasick), signal engine
src/sink     JSONL journal, alerts (stdout / Telegram / UDP)
src/app      stokd, stok-ref, stok-replay, settings
config/      stok.conf, rules.tsv
tests/       unit tests, fixtures, integration test (local HTTPS server)
```
