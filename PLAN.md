# Penny-Stock News Signal Bot: Feasibility and Build Plan

A StockTitan-style news aggregator that also flags small-cap and penny-stock news
that looks material, and fires a signal when the stock actually starts moving on it.

---

## TL;DR

| Question | Answer |
|---|---|
| Can we build a StockTitan-like aggregator plus alerts? | **Yes.** A useful personal version takes about 4–8 weeks of part-time work, and most of the data is free or cheap. |
| Can it become a bot that is **reliably profitable** on its own? | **Possible, but unlikely. Assume it won't be until your own data shows otherwise.** The easy edge (being first to a headline) goes to faster, better-funded players within seconds. In penny stocks, very positive-sounding news is often a warning sign rather than a buy signal. |
| What is the realistic win? | A filter that cuts about 1,000 headlines a day down to the 3–10 worth your attention, plus **measured statistics** on which catalysts actually lead to tradeable moves. Any trading edge would come from that dataset, not from raw sentiment. |
| How do we avoid losing money finding out? | Log everything, measure forward returns, paper-trade for 2–3 months against go/no-go criteria written down in advance, and only then trade small with hard risk limits. |

## Implementation status

The infrastructure is built as a C++20 engine that relies on external services as little as possible. See [README.md](README.md) and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

| Plan item | Status |
|---|---|
| Aggregator: EDGAR, wires, halts; dedupe; ticker mapping | Done (`stokd`, epoll/TLS poller, EDGAR exhibit fetch) |
| Enrichment from official sources | Done (`stok-ref`: Nasdaq Trader, SEC tickers/XBRL/daily indexes, FINRA) |
| Scoring | Done. A local rule engine (Aho-Corasick) replaces the LLM on the hot path. LLM enrichment stays optional and future, never blocking an alert. |
| Market confirmation | Done. Nasdaq ITCH 5.0 (MoldUDP64 / file), or a UDP bridge from any source. |
| Tiered alerts + halts + no-news movers | Done (stdout, Telegram, UDP JSON) |
| Outcome tracking / journal | Done (JSONL: news, signals, outcomes at +1/5/15/30/60 min) |
| Top of book + spread filter | Done (price ladders from ITCH order messages; bridge `Q` lines) |
| Paper trading + go/no-go report | Done (local simulator in the engine; `stok-report`) |
| MoldUDP64 gap recovery, company-name matching | Done |
| Historical backtest harness | Done (`stok-backtest`: recorded news × ITCH files / quote tapes, same engine and paper trader, sweeps with an out-of-sample split) |
| Recording + historical news | Done (`stokd` records ITCH/tapes; `stok-history` rebuilds past days' filings news from EDGAR archives) |

---

## 1. How realistic is a profitable bot?

### 1.1 What works in your favor

- **The data is accessible.** SEC EDGAR is free and close to real time. Most penny-stock press releases go out on a few wires (GlobeNewswire, PR Newswire, Accesswire, Newsfile, Business Wire). A Benzinga-grade news feed is now cheap through brokers and data vendors.
- **LLMs are good at structured extraction.** "Is this a definitive contract or a non-binding MOU? Is the counterparty named? What's the dollar value compared with the market cap?" used to need a quant team. Now it's one API call.
- **Penny-stock news moves are large.** Moves of +30% to +300% on a catalyst are routine, so a small real edge could matter.
- **Retail rule change.** The SEC approved removing the $25k pattern-day-trader minimum, effective June 4, 2026. Brokers have until Oct 20, 2027 to adopt the new intraday-margin framework, so check what your broker enforces today.

### 1.2 What works against you

| Problem | Why it hurts | What we do about it |
|---|---|---|
| **Speed** | HFTs and traders on paid low-latency feeds (Benzinga Pro squawk, direct wire feeds) react within milliseconds to seconds. Free RSS feeds can lag by a minute or more. By the time a retail bot sees "+20%", much of the move may be done. | Don't compete on the first tick. Look for **continuation setups** (the stock holds or reclaims VWAP after the first spike) and for **materiality the market hasn't priced yet**. Measure our own latency for every source. |
| **Positive tone ≠ positive return** | Microcaps issue promotional PRs ("poised", "transformational", "significant milestone"). Even StockTitan's own impact model, on its held-out test month, predicts the exact 1–5 impact bucket only 57% of the time, and impact doesn't tell you direction. | Score **substance**, not tone: definitive vs. LOI/MOU, named counterparty, dollar amount relative to market cap, revenue now vs. "potential". Penalize promotional language. |
| **Dilution** | Companies often use a spike to sell stock (ATM programs, registered directs, warrant exercises). The spike becomes the exit for insiders. | Track S-1, S-3, 424B*, EFFECT, and 8-K Item 3.02 filings, warrant overhang, and recent reverse splits. A dilution flag downgrades or blocks a signal. |
| **Liquidity, spreads, slippage** | Spreads of 2–10% and thin books. A backtest that fills at the last trade price is fiction. | Model fills at the ask plus slippage, after realistic latency. Require minimum dollar volume and a maximum spread. |
| **Halts** | LULD volatility halts and "news pending" halts are common. You can be stuck in a position, and it can reopen far lower. | Ingest the Nasdaq halt feed. Treat halt-resume as its own event type. Size positions for gap risk. |
| **Backtest self-deception** | Look-ahead bias (using the publish time instead of the time *you* could have seen it), survivorship bias (delisted tickers disappear), overfitting thresholds. | Record the time each item was *received*. Keep delisted tickers. Use walk-forward tests and a hold-out period that is never tuned on. |
| **Fraud and pumps** | OTC Pink and grey-market names, paid promotions, Discord/X pump groups. | Start with **exchange-listed only** (Nasdaq, NYSE, NYSE American). Keep a per-issuer history (PR frequency, past offerings, past reverse splits). |

### 1.3 Where a real edge *might* exist (hypotheses to test, not facts)

1. **Materiality relative to size.** A $20M purchase order for a $15M market-cap company should behave differently from the same headline at a $2B company.
2. **Catalyst type.** FDA approvals, definitive contracts with named large counterparties, and uplistings may behave very differently from "strategic partnership MOU", "AI/crypto-treasury pivot" or "letter to shareholders".
3. **Clean share structure plus low float plus high relative volume.** Fewer shares available and no near-term dilution path.
4. **Second-leg continuation** rather than the first spike: stocks that consolidate above VWAP 5–30 minutes after the news.
5. **An avoid list.** Knowing which "great news" to *never* chase may be worth more than any buy signal.

We'll know which of these (if any) hold only after Phase 4 (see §7).

### 1.4 Honest odds

- **As an alert tool you use to decide on trades yourself:** high chance it's genuinely useful.
- **As a fully automated trader that makes money after costs:** low chance. Most retail news bots end up flat or negative once slippage and fading spikes are counted. The plan is designed so that if the answer is "no edge", you find out on paper for about $100–300 in data costs instead of with your account.
- **Note:** the dependable money in this space comes from *selling the tool* (StockTitan runs on subscriptions), not from trading its signals. If the aggregator turns out well, that's a real option (see the legal notes in §10).

---

## 2. What we're building

Three layers, built in this order:

1. **Aggregator.** Ingests news and filings, deduplicates them, maps them to tickers, enriches them with market and share-structure data, and stores everything.
2. **Signal engine.** Scores the news (catalyst, materiality, red flags) and confirms it with price and volume. Sends tiered alerts.
3. **Evaluation loop.** Records what every item and every signal *would have* returned under realistic execution. This decides whether real money is ever involved.

A web dashboard (the StockTitan-like UI) is optional and comes after the signal engine proves useful.

---

## 3. Architecture

```
                 ┌────────────────────────── INGEST ──────────────────────────┐
  SEC EDGAR ────►│ edgar poller (Atom feed, 8-K/6-K/S-1/S-3/424B/EFFECT/Form 4) │
  Wire RSS  ────►│ wire pollers (GlobeNewswire, PRN, Accesswire, Newsfile, BW) │
  News API  ────►│ websocket (Benzinga via Alpaca or Massive)                  │
  Halts     ────►│ Nasdaq trade-halt feed                                      │
                 └───────────────┬────────────────────────────────────────────┘
                                 ▼
                 normalize → dedupe (same story on 3 wires) → ticker mapping
                                 ▼
                 ENRICH: price, market cap, float/shares out, avg volume,
                         dilution history (shelf/ATM/424B/3.02), reverse splits,
                         issuer PR frequency, exchange
                                 ▼
          ┌──────────── SCORE ─────────────┐     ┌────── MARKET ENGINE ──────┐
          │ fast rules (instant pre-score) │     │ live trades/quotes for    │
          │ rule scorer (LLM optional)     │     │ tickers with fresh news + │
          │ → news_score, flags            │     │ top-gainer scanner        │
          └──────────────┬─────────────────┘     │ → % move, RVOL, VWAP,     │
                         │                       │   spread, $ volume, halts │
                         └──────────┬────────────┴───────────┬──────────────┘
                                    ▼                        │
                         SIGNAL LOGIC (tiers, cooldowns) ◄───┘
                                    ▼
                  Telegram/stdout/UDP alert ── JSONL journal (everything) ── Dashboard (later)
                                    ▼
                  OUTCOME TRACKER: returns at +1m/+5m/+15m/+60m/close/next day,
                  max favorable/adverse excursion, simulated fills
```

### 3.1 Ingestion (sources)

| Source | What it gives | Latency | Cost | Notes |
|---|---|---|---|---|
| SEC EDGAR "latest filings" Atom feed + submissions API | 8-K, 6-K, S-1/S-3, 424B*, EFFECT, Form 4, 10-Q/K | Seconds to about a minute | Free | Max 10 req/s, and a User-Agent with contact info is required. `company_tickers.json` gives CIK↔ticker. XBRL `companyfacts` gives shares outstanding. |
| Wire RSS (GlobeNewswire, PR Newswire, Accesswire, Newsfile, Business Wire) | Raw press releases, usually with a "(NASDAQ: ABCD)" tag | Varies; can lag minutes | Free | Measure the lag against a paid feed before relying on these. |
| Benzinga news via Alpaca News API (websocket) | Real-time headlines and bodies, tagged with tickers | Seconds | Free tier with an Alpaca account | Best free starting point. |
| Benzinga news via Massive (formerly Polygon.io) | Same class of feed plus history | Seconds | Paid | Useful for **historical** news to backtest with. |
| Nasdaq Trader halts RSS | LULD and news-pending halts and resumes | Seconds | Free | |

### 3.2 Normalize, dedupe, map tickers

- Extract tickers with a regex on exchange tags (`\((NASDAQ|NYSE|NYSE American|OTC\w*):\s*([A-Z.]+)\)`), then with the ticker list from the news API, then by CIK for EDGAR filings.
- Dedupe by normalized title plus a similarity hash within a 30-minute window. Keep the **earliest received time** across sources.
- Store `published_at` (what the source claims) and `received_at` (when we saw it) separately. Backtests only ever use `received_at`.

### 3.3 Enrichment (per ticker, cached and refreshed daily)

- Last price, market cap, shares outstanding, float (approximate at first, from a paid source later), 30-day average volume and dollar volume, exchange.
- **Dilution profile:** active shelf (S-3), recent 424B5/424B4, EFFECT notices, 8-K 3.02 (unregistered sales), warrants mentioned in recent filings.
- **Distress profile:** 8-K 3.01 (listing deficiency), 8-K 5.03 or a proxy vote on a reverse split, going-concern language.
- **Issuer history:** PRs per month (a PR every few days is a promotion signal), past signals and their outcomes.

### 3.4 Scoring

**Step 1: fast rules (milliseconds).** Keyword and catalyst-type heuristics produce a rough pre-score so obvious junk is dropped immediately and obvious big news gets a live price subscription right away.

**Step 2: LLM extraction (1–5 s, runs in parallel).** One call per item that passed the universe filter, returning strict JSON, for example:

```json
{
  "catalyst_type": "contract | fda_approval | clinical_data | earnings | m_and_a_target | m_and_a_acquirer | uplisting | offering | reverse_split | partnership_mou | product_launch | crypto_or_ai_pivot | legal | other",
  "is_definitive": true,
  "counterparty_named": "Walmart Inc.",
  "counterparty_tier": "large_public | private | undisclosed",
  "dollar_value_usd": 20000000,
  "value_is_upper_bound": false,
  "revenue_timing": "immediate | within_12m | speculative",
  "dilution_mentioned": false,
  "promotional_language": 0.2,
  "novelty": "new | rehash_of_prior_pr",
  "one_line_summary": "Signed a 3-year $20M supply agreement with Walmart.",
  "bull_points": ["..."],
  "bear_points": ["..."]
}
```

We then compute `materiality = dollar_value / market_cap` in code, not in the LLM, and combine everything into a **news_score (0–100)** with explicit, versioned weights. Every weight change is logged so the backtest can tell which version produced which signal.

**Implemented differently.** The shipped engine does this extraction locally, with a compiled rule set (`config/rules.tsv`, about 320 phrases in one Aho-Corasick pass, ~11 µs per story): catalyst, definitive vs. non-binding, named counterparties, hedging and promotional language, offering and spam detection, and dollar-amount extraction for materiality. No external API is on the hot path. An LLM pass can be added later as asynchronous enrichment that never delays an alert.

### 3.5 Market engine ("is the stock actually running?")

- As soon as news arrives for a ticker, subscribe to live trades and quotes for it.
- Compute: % change from the last pre-news price, **relative volume** (compared with the same time of day on average), dollar volume, spread, position against VWAP, and halt status.
- Also run a **scanner without news**: top gainers by % move and relative volume, then look for their catalyst. Many runners have no news at all, which is itself useful information.

### 3.6 Signal logic (starting values, to be recalibrated in Phase 4)

**Universe:** Nasdaq, NYSE or NYSE American listing. Price $0.50–$10. Market cap under $500M. Excludes OTC for v1.

| Tier | Conditions (initial guesses) | Delivery |
|---|---|---|
| **WATCH** | news_score ≥ 60, no hard red flags | Dashboard and digest, no push |
| **ALERT** | WATCH, plus up ≥ 8% from the pre-news price, RVOL ≥ 5×, and ≥ $300k traded within 5 min, spread ≤ 3% | Push notification |
| **HIGH** | ALERT, plus news_score ≥ 80, float under 20M, no dilution flags, issuer PR frequency normal, holding above VWAP | Push notification, loud |

**Hard blocks:** offering or 424B in the last 5 trading days, an effective shelf plus an active ATM, a reverse split in the last 90 days, a known paid promotion, or a halt pending.

**Cooldowns:** one alert per ticker per catalyst, with re-alerts only on a tier upgrade.

An alert looks like:

```
🟠 ALERT  ABCD  $2.41 (+34% since 08:02 ET)   RVOL 18x   $4.1M traded
Contract · definitive · Walmart · $20M ≈ 1.3× market cap
Float 6.2M · no shelf · no offerings last 12m · 2 PRs last 30d
"Signed a 3-year $20M supply agreement with Walmart."
news_score 84 · received 08:02:11 ET (Accesswire) · 8-K pending
```

### 3.7 Storage

*As built:* an append-only JSONL journal per trading day (`news`, `signals`, `outcomes`), written by its own thread and queried with DuckDB or pandas. Raw story text and scores are kept, so history can be re-scored whenever the rules change. A database can be layered on later for a dashboard.

---

## 4. Tech stack (as built)

- **C++20**, single static core library, no third-party dependencies beyond **OpenSSL** (TLS) and **zlib** (gzip)
- Custom non-blocking HTTP/1.1 + TLS client on **epoll**: keep-alive, conditional GET, staggered lanes, per-host token buckets
- Lock-free **SPSC/MPSC rings**, per-symbol **seqlocks**, pinned threads, eventfd wake-ups or busy-polling
- **Nasdaq TotalView-ITCH 5.0** decoder with **MoldUDP64** receiver (the exchange's official protocol); file replay; UDP bridge
- **JSONL journal** instead of Postgres (append-only, off the hot path; query with DuckDB/pandas)
- **Backtester** that drives the production engine and paper trader on a simulated clock; no separate research code path to drift from live
- **Telegram Bot API** over a pre-warmed keep-alive connection; UDP JSON for dashboards
- Deployment: one VPS in US-East, systemd unit in `deploy/`

Layout: see the README.

## 5. Evaluation: the part that decides profitability

1. **Log everything, not just signals.** Every news item, every score, every price snapshot. Without the negatives we can't measure anything.
2. **Forward returns** from `received_at` at +1m, +5m, +15m, +60m, the close, and the next open and close. Also the max favorable and max adverse excursion.
3. **Realistic execution model:**
   - Entry at the **ask** at `received_at + latency` (test 5 s, 30 s and 120 s), plus slippage (e.g. 0.5–1% of price, more for wide spreads).
   - Exits: a fixed stop, a target, a trailing-VWAP exit and a time stop, all tested separately.
   - Skip trades where the stock was halted, or where the spread or dollar volume fails the filters.
4. **Bias controls:** `received_at` only, delisted tickers kept, parameters frozen before each walk-forward window, and **one hold-out month never used for tuning**.
5. **Metrics:** number of trades, win rate, average win and loss, **expectancy per trade after costs**, profit factor, max drawdown, and the result with the top 3 trades removed (so one lucky runner can't carry it).
6. **Pre-registered go/no-go for real money** (write these down *before* looking at the results):
   - ≥ 150 paper trades over ≥ 8 weeks
   - Positive expectancy after 1% round-trip slippage
   - Profit factor ≥ 1.3, still positive with the top 3 trades removed
   - Max drawdown you could stomach at 3× the planned size

If it fails, the tool is still a good alert and research platform. You just don't automate the trading.

---

## 6. Risk management (only relevant after Phase 5 passes)

- **Semi-automatic first:** the bot proposes a trade with a pre-filled order, and you tap to confirm. Full automation only after about a month of semi-automatic matching the paper results.
- Fixed small risk per trade (e.g. 0.5% of the account), a daily loss limit that disables trading for the day, and a maximum number of open positions.
- No averaging down. Limit orders only, never market orders in thin names. Be careful with pre-market liquidity.
- Kill switch (one command stops everything), plus a heartbeat alert if the bot goes silent.
- Broker: Alpaca (API-first, free paper trading) or Interactive Brokers. Confirm how your broker handles the new day-trading margin rules.

---

## 7. Phased roadmap

Assumes one developer working part-time. Weeks overlap.

| Phase | Weeks | Deliverable | Status |
|---|---|---|---|
| **0. Setup** | 0–1 | Repo, build, config, secrets via environment | **Done** |
| **1. Aggregator MVP** | 1–3 | EDGAR, wire RSS and halt ingestion, dedupe, ticker mapping, journal, alerts | **Done**. Run `stokd --probe` on your server to verify the feed URLs and measure latency. |
| **2. Scoring** | 3–5 | Rule scorer, enrichment (shares, float, dilution/distress flags), news_score | **Done**. Next: hand-label 300 releases from the journal and tune `rules.tsv`. |
| **3. Market confirmation** | 5–7 | ITCH/bridge market data, RVOL/VWAP, gainer scanner, tiered alerts | **Done**. Needs an ITCH subscription or a bridged feed for live prices. |
| **4. Outcome tracking and backtest** | 5–10 | Outcome tracker, `stok-report`, `stok-backtest` (recorded news × historical ITCH/tapes), `stok-history` (EDGAR-rebuilt news) | **Built**. Needs recorded days (or ITCH files) to say anything. |
| **5. Paper trading** | 10–20 | Paper orders from signals under fixed rules, weekly report | **Built** (`[paper]` + `stok-report`). Needs 8+ weeks of live data. |
| **6. Dashboard (optional)** | anytime after 3 | Web UI over the journal + UDP signal feed | Not started |
| **7. Small live trading** | only if Phase 5 passes | Semi-automatic execution with the risk limits from §6 | Not started |

Phases 1–3 give you a working alert tool in about 6–7 weeks. The profitability answer takes until about week 20, because it needs months of live-recorded data. Historical news with accurate receive-time timestamps is hard to get cheaply. EDGAR's acceptance timestamps (`stok-history`) are a free partial substitute for filed news, but wire-only stories and a paid ITCH history are the gaps.

---

## 8. Costs (monthly, approximate; verify current vendor pricing)

**LLM extraction.** The shipped engine scores locally, so there's no per-item API cost. For reference, if you later add optional LLM enrichment at about 300 in-universe items/day (about 9k/month), with about 2k input and 400 output tokens each:

| Model | $ per 1M in / out | ≈ per item | ≈ per month |
|---|---|---|---|
| `claude-opus-5` | $5 / $25 | $0.020 | ~$180 |
| `claude-sonnet-5` | $2 / $10 | $0.008 | ~$70 |
| `claude-haiku-4-5` | $1 / $5 | $0.004 | ~$35 |

**Everything:**

| Item | MVP (Phases 0–5) | Serious setup |
|---|---|---|
| News | Free (EDGAR, wire RSS, Alpaca news) | Paid Benzinga-grade feed: ~$100s |
| Market data | None (news-only), or replay Nasdaq's free sample ITCH files | Nasdaq TotalView-ITCH subscription + connectivity (exchange fees vary; check Nasdaq's price list), or bridge a broker feed |
| Historical bars and news for backtests | Massive or Databento: ~$30–200 while backfilling | Same |
| Float / short interest | Approximated from filings | Paid provider: ~$30–100 |
| LLM (optional enrichment only) | $0 | ~$35–180 |
| VPS | ~$10–20 | ~$20–50 |
| **Total** | **~$50–300** | **~$300–800** |

---

## 9. What's out of scope for v1

- OTC and Pink sheets (data quality, fraud, and different market data)
- Short selling or fading spikes (locate costs and squeeze risk; could be a v2 strategy, since many spikes fade)
- Options
- Social-media sentiment (X/Reddit/Stocktwits): noisy, expensive APIs and easy to manipulate. Maybe later as a *confirmation* feature.

---

## 10. Legal and compliance notes (not legal advice)

- **Personal use** of your own signals is fine.
- **Selling signals or the dashboard** can bring investment-adviser rules into play (the publisher exclusion covers impersonal, regularly published content). **News content and real-time exchange data carry redistribution and licensing fees** (display vs. non-display, professional vs. non-professional). Talk to a lawyer and your data vendors before charging anyone.
- Respect source terms of service and SEC EDGAR fair-access rules.
- Never post signals publicly in a way that promotes a stock you hold. In penny stocks that looks like a pump.

---

## 11. Decisions needed from you

1. **Mode:** alerts only (recommended to start) or eventual auto-trading?
2. **Budget:** OK to start on the free and ~$50–300/month tier?
3. **Hours:** pre-market (4:00–9:30 ET, when most small-cap PRs land) or regular hours only?
4. **Alert channel:** Telegram (default) or Discord?
5. **Broker for paper and live:** Alpaca (default) or IBKR?

## 12. Next step

Phases 0–4 are implemented, and Phase 5's paper trader is built (see *Implementation status* at the top). Next:

1. Deploy on a US-East VPS, set `net.user_agent`, run `stok-ref`, then `stokd --probe` to verify every feed URL and measure latency.
2. Run news-only for 1–2 weeks. Use the per-source "first/behind" stats to keep the fastest sources, and hand-label 300 stories from `news.jsonl` to tune `config/rules.tsv`.
3. Choose live market data: a Nasdaq ITCH subscription (the official path) or a bridged broker feed, then build the RVOL baseline.
4. Turn on `[paper]` and let it run. Check `stok-report` weekly. Only the go/no-go checklist, not a good week, decides about real money.
5. Keep `record_tape` (or `record_itch`) on, and backtest every week of recorded days with `stok-backtest`. Use `--news-delay-ms` to see how much of the result depends on being early, and `--sweep … --split` to tune on older days and judge on newer ones.
6. For an early read before live data accumulates, rebuild a few past days with `stok-history` and backtest them on Nasdaq's free ITCH sample days. Treat that as a smoke test, not evidence: a handful of days is far below the 150-trade bar.
