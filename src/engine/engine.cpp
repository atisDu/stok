#include "engine/engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/clock.hpp"
#include "core/log.hpp"
#include "feeds/parsers.hpp"
#include "util/time.hpp"

namespace stok {

const char* tier_name(Tier t) {
  switch (t) {
    case Tier::None: return "none";
    case Tier::Watch: return "WATCH";
    case Tier::Alert: return "ALERT";
    case Tier::High: return "HIGH";
    case Tier::Halt: return "HALT";
    case Tier::Mover: return "MOVER";
    case Tier::Info: return "INFO";
  }
  return "?";
}

double expected_volume_fraction(int minute) {
  // Cumulative share of regular-session volume at 30-minute marks from 09:30
  // to 16:00 (the typical U-shape: heavy open, quiet midday, heavy close).
  static const double kRth[14] = {0.0, 0.13, 0.22, 0.29, 0.35, 0.40, 0.45, 0.49, 0.53, 0.58, 0.63, 0.69, 0.78, 1.0};
  constexpr int kPre = 4 * 60, kOpen = 9 * 60 + 30, kClose = 16 * 60, kPost = 20 * 60;
  if (minute < kPre) return 0.0;
  if (minute < kOpen) return 0.04 * (minute - kPre) / static_cast<double>(kOpen - kPre);
  if (minute < kClose) {
    const double x = (minute - kOpen) / 30.0;
    const int i = std::min(12, static_cast<int>(x));
    const double g = kRth[i] + (kRth[i + 1] - kRth[i]) * (x - i);
    return 0.04 + 0.92 * g;
  }
  if (minute < kPost) return 0.96 + 0.04 * (minute - kClose) / static_cast<double>(kPost - kClose);
  return 1.0;
}

Engine::Engine(EngineConfig cfg, const SymbolTable& symbols, const MarketBoard* board, const Scorer& scorer,
               FilingsHistory& filings, SpscQueue<Signal>* alerts, Waker* alert_waker,
               SpscQueue<JournalRecord>* journal, Waker* journal_waker, std::vector<std::string> source_names)
    : cfg_(std::move(cfg)),
      symbols_(symbols),
      board_(board),
      scorer_(scorer),
      filings_(filings),
      alerts_(alerts),
      alert_waker_(alert_waker),
      journal_(journal),
      journal_waker_(journal_waker),
      source_names_(std::move(source_names)) {
  watch_.resize(1024);
  src_stats_.resize(std::max<std::size_t>(1, source_names_.size() + 1));
  universe_static_.assign(symbols_.size(), 0);
  for (uint32_t i = 0; i < symbols_.size(); ++i) {
    const SymbolInfo& s = symbols_[i];
    bool ok = !s.etf && !s.test_issue && !s.is_derivative_security();
    if (cfg_.listed_only) ok = ok && is_exchange_listed(s.exchange);
    universe_static_[i] = ok ? 1 : 0;
  }
}

int32_t Engine::today_days(uint64_t now_wall) {
  if (now_wall >= cached_day_until_ || cached_day_ == 0) {
    int y;
    unsigned m, d;
    timeutil::eastern_date(static_cast<int64_t>(now_wall), y, m, d);
    cached_day_ = static_cast<int32_t>(timeutil::days_from_civil(y, m, d));
    cached_day_until_ = now_wall + 60 * kNsPerSec;
  }
  return cached_day_;
}

bool Engine::in_universe(uint32_t sym, double price, double* mcap_out) const {
  if (mcap_out) *mcap_out = 0;
  if (sym >= universe_static_.size() || !universe_static_[sym]) return false;
  const SymbolInfo& s = symbols_[sym];
  if (price > 0) {
    if (price < cfg_.min_price || price > cfg_.max_price) return false;
  } else if (!cfg_.allow_unknown_price) {
    return false;
  }
  const double mcap = (s.shares_outstanding > 0 && price > 0) ? s.shares_outstanding * price : 0;
  if (mcap_out) *mcap_out = mcap;
  if (mcap > 0 && mcap > cfg_.max_market_cap) return false;
  return true;
}

double Engine::rvol_of(uint32_t sym, uint64_t day_volume, uint64_t now_wall) const {
  const double adv = symbols_[sym].adv20;
  if (adv <= 0) return 0;
  const double frac = expected_volume_fraction(timeutil::eastern_minute_of_day(static_cast<int64_t>(now_wall)));
  const double expected = std::max(adv * frac, adv * 0.002);
  return static_cast<double>(day_volume) / expected;
}

uint32_t Engine::soft_penalty_flags() const {
  return kFlagShelf | kFlagS1Pending | kFlagReverseSplitRisk | kFlagDeficiency | kFlagLateFiler |
         kFlagReverseSplitDone;
}

bool Engine::cooled_down(uint32_t sym, Tier t, uint64_t now) {
  const uint64_t key = (static_cast<uint64_t>(sym) << 3) | static_cast<uint64_t>(t);
  auto [last, inserted] = last_tier_ns_.try_emplace(key);
  if (!inserted && now - *last < static_cast<uint64_t>(cfg_.cooldown_s) * kNsPerSec) return false;
  *last = now;
  return true;
}

Engine::Watch* Engine::find_watch(uint32_t sym) {
  for (auto& w : watch_)
    if (w.active && w.sym == sym) return &w;
  return nullptr;
}

Engine::Watch* Engine::alloc_watch() {
  for (auto& w : watch_)
    if (!w.active) return &w;
  // Full: evict the oldest.
  Watch* oldest = &watch_[0];
  for (auto& w : watch_)
    if (w.t_news < oldest->t_news) oldest = &w;
  return oldest;
}

std::size_t Engine::watch_count() const {
  std::size_t n = 0;
  for (const auto& w : watch_) n += w.active ? 1 : 0;
  return n;
}

void Engine::build_why(Watch& w, const ScoreResult& r, double mcap) {
  char buf[192];
  int n = std::snprintf(buf, sizeof(buf), "%s", catalyst_name(r.catalyst));
  int shown = 0;
  for (int i = 0; i < r.n_matched && shown < 3 && n < static_cast<int>(sizeof(buf)) - 8; ++i) {
    const auto& rule = scorer_.rule(r.matched[i]);
    if (rule.kind_str == "hedge" || rule.kind_str == "promo") continue;
    n += std::snprintf(buf + n, sizeof(buf) - static_cast<std::size_t>(n), "%s%s", shown ? ", " : " | ",
                       rule.phrase.c_str());
    ++shown;
  }
  if (r.amount_usd > 0 && n < static_cast<int>(sizeof(buf)) - 32) {
    if (mcap > 0)
      n += std::snprintf(buf + n, sizeof(buf) - static_cast<std::size_t>(n), " | $%.1fM = %.2fx mcap",
                         r.amount_usd / 1e6, r.amount_usd / mcap);
    else
      n += std::snprintf(buf + n, sizeof(buf) - static_cast<std::size_t>(n), " | $%.1fM", r.amount_usd / 1e6);
  }
  if ((r.hedges || r.promos) && n < static_cast<int>(sizeof(buf)) - 24)
    n += std::snprintf(buf + n, sizeof(buf) - static_cast<std::size_t>(n), " | hedge %d promo %d", r.hedges, r.promos);
  w.why.assign(std::string_view(buf, static_cast<std::size_t>(std::max(0, std::min(n, static_cast<int>(sizeof(buf)) - 1)))));
}

void Engine::emit(const Watch& w, Tier tier, uint64_t now_wall, const char* halt_reason) {
  Signal s{};
  s.tier = tier;
  s.catalyst = w.catalyst;
  s.source = w.source;
  s.sym = w.sym;
  s.score = w.score;
  s.score_flags = w.score_flags;
  s.dilution_flags = w.dilution_flags;
  s.news_id = w.news_id;
  s.t_news_recv_ns = w.t_news;
  s.t_signal_ns = wall_ns();
  s.published_ns = w.published_ns;
  s.amount_usd = w.amount_usd;
  s.materiality = w.materiality;
  s.market_cap = w.market_cap;
  const SymbolInfo& info = symbols_[w.sym];
  s.shares_out = info.shares_outstanding;
  std::snprintf(s.ticker, sizeof(s.ticker), "%s", info.ticker.c_str());
  std::snprintf(s.exchange, sizeof(s.exchange), "%s", exchange_name(info.exchange));
  if (halt_reason) std::snprintf(s.halt_reason, sizeof(s.halt_reason), "%s", halt_reason);
  if (board_) {
    const MarketHot h = board_->hot(w.sym);
    s.price = h.last();
    s.ref_price = w.ref_px / 1e4;
    if (w.ref_px > 0 && h.last_px > 0) s.move_pct = (h.last_px - w.ref_px) * 100.0 / w.ref_px;
    s.rvol = rvol_of(w.sym, h.day_volume, now_wall);
    s.dollar_volume = static_cast<double>(h.notional_e4 - std::min(h.notional_e4, w.notional_at_news)) / 1e4;
    s.vwap = h.vwap();
    if (s.market_cap <= 0 && info.shares_outstanding > 0 && s.price > 0) s.market_cap = info.shares_outstanding * s.price;
  } else {
    s.price = info.prev_close;
  }
  s.why = w.why;
  s.title = w.title;
  s.link = w.link;
  ++tier_counts_[static_cast<int>(tier)];
  if (w.t_news && s.t_signal_ns > w.t_news) {
    // WATCH is emitted while handling the news: this is pure processing
    // latency. ALERT/HIGH wait for the market to react: that is market time.
    if (tier == Tier::Watch) recv_to_watch_ns_.record(s.t_signal_ns - w.t_news);
    else if (tier == Tier::Alert || tier == Tier::High) e2e_ns_.record(s.t_signal_ns - w.t_news);
  }

  const bool push = tier != Tier::Watch || cfg_.push_watch;
  if (push && alerts_) {
    if (alerts_->try_push(s)) {
      if (alert_waker_) alert_waker_->notify();
    } else {
      ++alert_drops_;
    }
  }
  if (journal_) {
    if (JournalRecord* r = journal_->try_claim()) {
      r->type = JournalRecord::Type::Signal;
      r->signal = s;
      journal_->publish();
      if (journal_waker_) journal_waker_->notify();
    } else {
      ++journal_drops_;
    }
  }
}

void Engine::journal_outcome(const Watch& w, uint32_t horizon_s, uint64_t now_wall, const MarketHot& h, double move) {
  if (!journal_) return;
  JournalRecord* r = journal_->try_claim();
  if (!r) {
    ++journal_drops_;
    return;
  }
  r->type = JournalRecord::Type::Outcome;
  Outcome& o = r->outcome;
  o = Outcome{};
  o.sym = w.sym;
  o.horizon_s = horizon_s;
  o.news_id = w.news_id;
  o.t_news_recv_ns = w.t_news;
  o.t_ns = now_wall;
  o.ref_price = w.ref_px / 1e4;
  o.price = h.last();
  o.move_pct = move;
  o.max_move_pct = w.max_move;
  o.min_move_pct = w.min_move;
  o.volume_since = static_cast<double>(h.day_volume - std::min(h.day_volume, w.vol_at_news));
  o.dollar_volume_since = static_cast<double>(h.notional_e4 - std::min(h.notional_e4, w.notional_at_news)) / 1e4;
  o.score = w.score;
  o.tier_reached = w.tier;
  o.catalyst = w.catalyst;
  std::snprintf(o.ticker, sizeof(o.ticker), "%s", symbols_[w.sym].ticker.c_str());
  journal_->publish();
  if (journal_waker_) journal_waker_->notify();
}

void Engine::on_news(const NewsEvent& ev, uint64_t now) {
  const uint64_t t0 = mono_ns();
  SourceStats& ss = src_stats_[std::min<std::size_t>(ev.source, src_stats_.size() - 1)];
  ++ss.items;
  if (ev.parsed_ns && now > ev.parsed_ns) ss.queue_ns.record(now - ev.parsed_ns);
  if (ev.published_ns > 0 && ev.recv_ns > static_cast<uint64_t>(ev.published_ns))
    ss.publish_lag.record(ev.recv_ns - static_cast<uint64_t>(ev.published_ns));

  // Cross-source dedupe by normalized headline: who had it first, and by how much.
  bool dup = false;
  uint16_t first_src = ev.source;
  int64_t lag = 0;
  if (ev.title_key && ev.kind != EventKind::Halt) {
    auto [fs, inserted] = first_seen_.try_emplace(ev.title_key);
    if (inserted) {
      *fs = FirstSeen{ev.source, ev.recv_ns};
      ++ss.first;
    } else {
      dup = true;
      first_src = fs->source;
      lag = static_cast<int64_t>(ev.recv_ns) - static_cast<int64_t>(fs->recv_ns);
      if (fs->source != ev.source) {
        ++ss.dup;
        if (lag > 0) ss.dup_lag.record(static_cast<uint64_t>(lag));
      }
    }
  }

  auto journal_news = [&](const ScoreResult& r, uint32_t sym, uint32_t dil, double mcap, double price, bool inu) {
    if (!journal_) return;
    JournalRecord* rec = journal_->try_claim();
    if (!rec) {
      ++journal_drops_;
      return;
    }
    rec->type = JournalRecord::Type::News;
    rec->news = ev;
    rec->score = r;
    rec->news_sym = sym;
    rec->news_dilution = dil;
    rec->news_market_cap = mcap;
    rec->news_price = price;
    rec->news_in_universe = inu;
    rec->news_duplicate = dup;
    rec->first_source = first_src;
    rec->first_seen_lag_ns = lag;
    rec->engine_ns = now;
    journal_->publish();
    if (journal_waker_) journal_waker_->notify();
  };

  if (ev.kind == EventKind::Halt) {
    const bool resumed = ev.halt.resume_trade_ns != 0;
    for (int i = 0; i < ev.n_tickers; ++i)
      handle_halt(ev.tickers[i], !resumed, ev.halt.reason, ev.halt.halt_ns, false, now, ev.title.c_str());
    journal_news(ScoreResult{}, ev.n_tickers ? ev.tickers[0] : SymbolTable::kInvalid, 0, 0, 0, false);
    return;
  }

  ScoreResult r;
  if (ev.kind == EventKind::Filing) {
    const FormClass cls = classify_form(ev.form);
    filings_.add(ev.cik, today_days(now), cls, ev.items_mask);
    if (cls != FormClass::Form8K && cls != FormClass::Form6K) {
      // Registration / prospectus / proxy: updates dilution state. If the
      // issuer is on the watchlist, tell the trader right away.
      if (cls == FormClass::Prospectus || cls == FormClass::Effect || cls == FormClass::S1 || cls == FormClass::S3) {
        for (int i = 0; i < ev.n_tickers; ++i) {
          if (Watch* w = find_watch(ev.tickers[i])) {
            Watch info = *w;
            info.title.assign(ev.title.view());
            info.link.assign(ev.link.view());
            char why[96];
            std::snprintf(why, sizeof(why), "dilution filing on a watched ticker: %s", ev.form);
            info.why.assign(why);
            w->dilution_flags |= cls == FormClass::Prospectus ? kFlagOfferingRecent : kFlagEffectRecent;
            emit(info, Tier::Info, now);
          }
        }
      }
      journal_news(r, ev.n_tickers ? ev.tickers[0] : SymbolTable::kInvalid, 0, 0, 0, false);
      return;
    }
    r = scorer_.score_text(ev.title.view(), ev.body.view());
    if (ev.items_mask & kItem302) r.flags |= kSfOffering;
    if (ev.items_mask & (kItem103 | kItem301 | kItem402)) r.flags |= kSfDistress;
    // Item-based floor when the item titles alone matched no rule. The
    // exhibit fetch usually follows with the full press release text.
    if (r.catalyst == Catalyst::None) {
      struct ItemFloor {
        uint32_t mask;
        Catalyst cat;
        int score;
      };
      static constexpr ItemFloor kFloors[] = {{kItem501, Catalyst::MnaTarget, 45},
                                              {kItem101, Catalyst::Contract, 45},
                                              {kItem202, Catalyst::Earnings, 30},
                                              {kItem701 | kItem801, Catalyst::Other, 20}};
      for (const auto& f : kFloors) {
        if (ev.items_mask & f.mask) {
          r.catalyst = f.cat;
          r.text_score = std::max(r.text_score, f.score);
          break;
        }
      }
    }
    if (r.flags & kSfOffering) r.text_score = std::min(r.text_score, 15);
    if (r.flags & kSfDistress) r.text_score = std::max(0, r.text_score - 25);
    r.score = r.text_score;
  } else {
    r = scorer_.score_text(ev.title.view(), ev.body.view());
  }
  ++ss.scored;

  const int32_t today = today_days(now);
  const int n = (ev.kind == EventKind::News) ? ev.n_tickers : std::min<int>(1, ev.n_tickers);
  uint32_t p_sym = SymbolTable::kInvalid, p_dil = 0;
  double p_mcap = 0, p_price = 0;
  bool p_inu = false, counted_universe = false;
  ScoreResult p_r = r;
  bool touched_watch = false;
  for (int i = 0; i < n; ++i) {
    const uint32_t sym = ev.tickers[i];
    if (sym >= symbols_.size()) continue;
    const SymbolInfo& info = symbols_[sym];
    const MarketHot hot = board_ ? board_->hot(sym) : MarketHot{};
    const double price = hot.last_px > 0 ? hot.last() : info.prev_close;
    double mcap = 0;
    const bool inu = in_universe(sym, price, &mcap);
    ScoreResult rs = r;
    Scorer::finalize(rs, mcap);
    const uint32_t dil = filings_.flags(info.cik, today, cfg_.dilution) | (info.distressed() ? kFlagDeficiency : 0u);
    int pen = 0;
    if (dil & kFlagShelf) pen += 3;
    if (dil & kFlagS1Pending) pen += 5;
    if (dil & kFlagReverseSplitRisk) pen += 5;
    if (dil & kFlagReverseSplitDone) pen += 5;
    if (dil & kFlagDeficiency) pen += 8;
    if (dil & kFlagLateFiler) pen += 5;
    rs.score = std::clamp(rs.score - pen, 0, 100);
    const bool hard_block = (rs.flags & (kSfOffering | kSfSpam)) ||
                            (dil & (kFlagOfferingRecent | kFlagEffectRecent | kFlagUnregisteredSale));
    if (i == 0) {
      p_sym = sym;
      p_r = rs;
      p_dil = dil;
      p_mcap = mcap;
      p_price = price;
      p_inu = inu;
    }
    if (inu && !counted_universe) {
      ++ss.in_universe;
      counted_universe = true;
    }
    last_news_by_sym_.insert_or_assign(sym, now);
    if (!inu || hard_block || rs.score < cfg_.watch_score) continue;

    Watch* w = find_watch(sym);
    if (w && rs.score <= w->score) continue;  // keep the earlier/stronger story
    const bool fresh = w == nullptr;
    if (fresh) {
      w = alloc_watch();
      *w = Watch{};
      w->active = true;
      w->sym = sym;
      w->t_news = ev.recv_ns ? ev.recv_ns : now;
      w->ref_px = hot.last_px > 0 ? hot.last_px : static_cast<int32_t>(std::lround(info.prev_close * 1e4));
      w->vol_at_news = hot.day_volume;
      w->notional_at_news = hot.notional_e4;
    }
    w->source = ev.source;
    w->score = rs.score;
    w->catalyst = rs.catalyst;
    w->score_flags = rs.flags;
    w->dilution_flags = dil;
    w->news_id = ev.id_hash;
    w->published_ns = ev.published_ns;
    w->amount_usd = rs.amount_usd;
    w->materiality = rs.materiality;
    w->market_cap = mcap;
    w->title = ev.title;
    w->link = ev.link;
    build_why(*w, rs, mcap);
    ++ss.watch;
    if (w->tier == Tier::None) {
      w->tier = Tier::Watch;
      if (cooled_down(sym, Tier::Watch, now)) emit(*w, Tier::Watch, now);
    }
    touched_watch = true;
  }
  journal_news(p_r, p_sym, p_dil, p_mcap, p_price, p_inu);
  handle_ns_.record(mono_ns() - t0);
  // The stock may already be running (news seen late, or the market beat
  // us): evaluate immediately instead of waiting for the next tick.
  if (touched_watch) evaluate(now);
}

void Engine::handle_halt(uint32_t sym, bool halted, const char* reason, int64_t ts, bool from_itch, uint64_t now,
                         const char* title) {
  if (sym >= symbols_.size()) return;
  const char state = halted ? 'H' : 'T';
  auto [hs, inserted] = halt_seen_.try_emplace(sym);
  if (!inserted && hs->state == state && now - hs->t < 120 * kNsPerSec) return;  // other source already reported it
  *hs = HaltSeen{state, now};
  Watch* w = find_watch(sym);
  bool alert = false;
  if (cfg_.halt_alerts == 2) {
    const MarketHot h = board_ ? board_->hot(sym) : MarketHot{};
    alert = in_universe(sym, h.last_px > 0 ? h.last() : symbols_[sym].prev_close);
  } else if (cfg_.halt_alerts == 1) {
    alert = w != nullptr;
  }
  if (!alert) return;
  Watch tmp = w ? *w : Watch{};
  if (!w) {
    tmp.sym = sym;
    tmp.t_news = now;
  }
  char buf[256];
  if (title && *title) {
    std::snprintf(buf, sizeof(buf), "%s", title);
  } else {
    std::snprintf(buf, sizeof(buf), "%s %s (%s: %s) from the market-data feed", halted ? "HALT" : "RESUME",
                  symbols_[sym].ticker.c_str(), reason && *reason ? reason : "-",
                  reason && *reason ? halt_reason_desc(reason) : "trading state change");
  }
  tmp.title.assign(buf);
  char why[128];
  std::snprintf(why, sizeof(why), "%s%s", halted ? "halted" : "resumed", w ? " | on watchlist" : "");
  tmp.why.assign(why);
  (void)ts;
  emit(tmp, Tier::Halt, now, reason);
}

void Engine::on_market_event(const MarketEvent& ev, uint64_t now) {
  if (ev.type == MarketEvent::Type::TradingAction) {
    handle_halt(ev.sym, ev.state != 'T', ev.reason, ev.ts_ns, true, now, nullptr);
  } else if (ev.type == MarketEvent::Type::SystemEvent) {
    LOG_INFO("market system event '%c'", ev.sys_code);
  }
}

void Engine::evaluate(uint64_t now) {
  const uint64_t window = static_cast<uint64_t>(cfg_.watch_window_s) * kNsPerSec;
  for (auto& w : watch_) {
    if (!w.active) continue;
    const uint64_t age = now > w.t_news ? now - w.t_news : 0;
    if (age > window) {
      w.active = false;
      continue;
    }
    if (!board_) continue;
    const MarketHot h = board_->hot(w.sym);
    if (h.last_px <= 0) continue;
    if (w.ref_px <= 0) w.ref_px = h.last_px;  // first print after the news
    const double move = (h.last_px - w.ref_px) * 100.0 / w.ref_px;
    w.max_move = std::max(w.max_move, move);
    w.min_move = std::min(w.min_move, move);
    for (std::size_t k = 0; k < cfg_.outcome_horizons_s.size() && k < 32; ++k) {
      if (w.horizons_done & (1u << k)) continue;
      if (age >= static_cast<uint64_t>(cfg_.outcome_horizons_s[k]) * kNsPerSec) {
        w.horizons_done |= 1u << k;
        journal_outcome(w, static_cast<uint32_t>(cfg_.outcome_horizons_s[k]), now, h, move);
      }
    }
    if (w.tier >= Tier::High) continue;
    const double dollar = static_cast<double>(h.notional_e4 - std::min(h.notional_e4, w.notional_at_news)) / 1e4;
    const double rvol = rvol_of(w.sym, h.day_volume, now);
    const bool rvol_ok = rvol <= 0 || rvol >= cfg_.alert_rvol;  // unknown baseline doesn't block
    if (!(move >= cfg_.alert_move_pct && dollar >= cfg_.alert_dollar_volume && rvol_ok && !h.halted())) continue;
    const SymbolInfo& info = symbols_[w.sym];
    const bool above_vwap = h.last() >= h.vwap();
    const bool high = w.score >= cfg_.high_score && info.shares_outstanding > 0 &&
                      info.shares_outstanding <= cfg_.high_max_shares && (w.dilution_flags & soft_penalty_flags()) == 0 &&
                      (!cfg_.high_require_above_vwap || above_vwap);
    const Tier t = high ? Tier::High : Tier::Alert;
    if (t > w.tier) {
      w.tier = t;
      if (cooled_down(w.sym, t, now)) emit(w, t, now);
    }
  }
}

void Engine::scan_movers(uint64_t now) {
  if (!board_ || !cfg_.movers_enabled) return;
  const uint64_t day = static_cast<uint64_t>(today_days(now));
  const std::size_t n = std::min(board_->size(), symbols_.size());
  for (uint32_t sym = 0; sym < n; ++sym) {
    if (!universe_static_[sym]) continue;
    const MarketHot h = board_->hot(sym);
    if (h.last_px <= 0 || h.halted()) continue;
    const SymbolInfo& info = symbols_[sym];
    if (info.prev_close <= 0) continue;
    const double price = h.last();
    const double move = (price - info.prev_close) * 100.0 / info.prev_close;
    if (move < cfg_.mover_move_pct) continue;
    const double dollar = static_cast<double>(h.notional_e4) / 1e4;
    if (dollar < cfg_.mover_dollar_volume) continue;
    const double rvol = rvol_of(sym, h.day_volume, now);
    if (rvol > 0 && rvol < cfg_.mover_rvol) continue;
    if (!in_universe(sym, price)) continue;
    if (find_watch(sym)) continue;
    auto [seen, inserted] = mover_alerted_.try_emplace(sym);
    if (!inserted && *seen == day) continue;
    *seen = day;
    Watch tmp;
    tmp.sym = sym;
    tmp.t_news = now;
    tmp.ref_px = static_cast<int32_t>(std::lround(info.prev_close * 1e4));
    char title[200];
    const uint64_t* last_news = last_news_by_sym_.find(sym);
    if (last_news && now - *last_news < 24 * 3600 * kNsPerSec)
      std::snprintf(title, sizeof(title), "%s +%.0f%% vs prev close (news %llu min ago, below threshold)",
                    info.ticker.c_str(), move, static_cast<unsigned long long>((now - *last_news) / (60 * kNsPerSec)));
    else
      std::snprintf(title, sizeof(title), "%s +%.0f%% vs prev close, no news found", info.ticker.c_str(), move);
    tmp.title.assign(title);
    char why[128];
    std::snprintf(why, sizeof(why), "mover | $%.1fM traded | rvol %.1f", dollar / 1e6, rvol);
    tmp.why.assign(why);
    emit(tmp, Tier::Mover, now);
  }
}

void Engine::run(SpscQueue<NewsEvent>& news, SpscQueue<MarketEvent>& market, Waker& self_waker,
                 const std::atomic<bool>& stop) {
  const uint64_t eval_every = board_ ? cfg_.eval_interval_ns : kNsPerSec;
  const uint64_t scan_every = static_cast<uint64_t>(std::max(100, cfg_.mover_scan_ms)) * kNsPerMs;
  const uint64_t stats_every = static_cast<uint64_t>(std::max(5, cfg_.stats_interval_s)) * kNsPerSec;
  uint64_t next_eval = mono_ns() + eval_every;
  uint64_t next_scan = mono_ns() + scan_every;
  uint64_t next_stats = mono_ns() + stats_every;
  while (!stop.load(std::memory_order_relaxed)) {
    int did = 0;
    while (NewsEvent* ev = news.front()) {
      on_news(*ev, wall_ns());
      news.pop();
      ++did;
    }
    while (MarketEvent* me = market.front()) {
      on_market_event(*me, wall_ns());
      market.pop();
      ++did;
    }
    const uint64_t now = mono_ns();
    if (now >= next_eval) {
      evaluate(wall_ns());
      next_eval = now + eval_every;
    }
    if (board_ && cfg_.movers_enabled && now >= next_scan) {
      scan_movers(wall_ns());
      next_scan = now + scan_every;
    }
    if (now >= next_stats) {
      next_stats = now + stats_every;
      const std::string rep = stats_report(true);
      LOG_INFO("engine stats:\n%s", rep.c_str());
    }
    if (did) continue;
    if (cfg_.busy_poll) {
      cpu_relax();
      continue;
    }
    uint64_t until = std::min(next_eval, next_stats);
    if (board_ && cfg_.movers_enabled) until = std::min(until, next_scan);
    const uint64_t t = mono_ns();
    if (until > t) self_waker.wait(until - t, [&] { return !news.empty() || !market.empty(); });
  }
}

std::string Engine::stats_report(bool reset) {
  std::string out;
  char line[512];
  auto ms = [](uint64_t ns) { return static_cast<double>(ns) / 1e6; };
  for (std::size_t i = 0; i < src_stats_.size(); ++i) {
    SourceStats& s = src_stats_[i];
    if (s.items == 0) continue;
    const char* name = i < source_names_.size() ? source_names_[i].c_str() : "?";
    std::snprintf(line, sizeof(line),
                  "  %-18s items=%llu first=%llu behind=%llu (p50 %.0fms) universe=%llu watch=%llu | queue p50=%.1fus "
                  "p99=%.1fus | publish->recv p50=%.1fs\n",
                  name, (unsigned long long)s.items, (unsigned long long)s.first, (unsigned long long)s.dup,
                  ms(s.dup_lag.percentile(50)), (unsigned long long)s.in_universe, (unsigned long long)s.watch,
                  static_cast<double>(s.queue_ns.percentile(50)) / 1e3,
                  static_cast<double>(s.queue_ns.percentile(99)) / 1e3,
                  static_cast<double>(s.publish_lag.percentile(50)) / 1e9);
    out += line;
    if (reset) {
      s.items = s.first = s.dup = s.scored = s.in_universe = s.watch = 0;
      s.queue_ns.reset();
      s.publish_lag.reset();
      s.dup_lag.reset();
    }
  }
  std::snprintf(line, sizeof(line),
                "  engine: handle p50=%.1fus max=%.1fus | response->WATCH p50=%.3fms p99=%.3fms | news->price "
                "confirmation p50=%.0fms | watch=%zu | signals W=%llu A=%llu H=%llu halt=%llu mover=%llu info=%llu | "
                "drops journal=%llu alert=%llu\n",
                static_cast<double>(handle_ns_.percentile(50)) / 1e3, static_cast<double>(handle_ns_.max()) / 1e3,
                ms(recv_to_watch_ns_.percentile(50)), ms(recv_to_watch_ns_.percentile(99)), ms(e2e_ns_.percentile(50)),
                watch_count(), (unsigned long long)tier_counts_[1],
                (unsigned long long)tier_counts_[2], (unsigned long long)tier_counts_[3],
                (unsigned long long)tier_counts_[4], (unsigned long long)tier_counts_[5],
                (unsigned long long)tier_counts_[6], (unsigned long long)journal_drops_,
                (unsigned long long)alert_drops_);
  out += line;
  if (reset) {
    handle_ns_.reset();
    e2e_ns_.reset();
    recv_to_watch_ns_.reset();
  }
  return out;
}

}  // namespace stok
