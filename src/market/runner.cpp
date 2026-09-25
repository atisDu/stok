#include "market/runner.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

#include "core/ascii.hpp"
#include "core/clock.hpp"
#include "core/log.hpp"
#include "util/time.hpp"

namespace stok {

std::string itch_file_date(const std::string& path) {
  const auto slash = path.find_last_of('/');
  const std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
  if (base.size() < 8) return {};
  for (int i = 0; i < 8; ++i)
    if (!is_digit(base[static_cast<std::size_t>(i)])) return {};
  return base.substr(4, 4) + "-" + base.substr(0, 2) + "-" + base.substr(2, 2);
}

MarketRunner::MarketRunner(MarketOptions opts, const SymbolTable& symbols, MarketBoard& board,
                           SpscQueue<MarketEvent>& events, Waker* engine_waker)
    : opts_(std::move(opts)), symbols_(symbols), board_(board), events_(events), waker_(engine_waker) {}

bool MarketRunner::init(std::string* err) {
  date_ = opts_.session_date;
  if (date_.empty() && opts_.source == "itch_file") date_ = itch_file_date(opts_.itch_file);
  if (date_.empty()) date_ = timeutil::eastern_date_str(static_cast<int64_t>(wall_ns()));
  int y;
  unsigned m, d;
  if (!timeutil::parse_ymd(date_, y, m, d)) {
    if (err) *err = "bad session date " + date_;
    return false;
  }
  board_.set_session_date(y, m, d);
  book_ = std::make_unique<ItchBook>(symbols_, board_, &events_, waker_, opts_.order_capacity);

  if (opts_.source == "itch_file") {
    file_ = std::make_unique<ItchFileReader>();
    if (!file_->open(opts_.itch_file, err)) return false;
  } else if (opts_.source == "moldudp64") {
    mold_ = std::make_unique<MoldUdp64Receiver>();
    auto mo = opts_.mold;
    mo.busy_poll = opts_.busy_poll;
    if (!mold_->open(mo, err)) return false;
  } else if (opts_.source == "bridge") {
    bridge_ = std::make_unique<BridgeReceiver>();
    if (!bridge_->open(opts_.bridge_bind, opts_.bridge_port, err)) return false;
  } else if (opts_.source != "none") {
    if (err) *err = "unknown market.source '" + opts_.source + "'";
    return false;
  }
  return true;
}

void MarketRunner::on_bridge_state(void* ctx, uint32_t sym, char state, const char* reason, int64_t ts) {
  auto* self = static_cast<MarketRunner*>(ctx);
  if (MarketEvent* e = self->events_.try_claim()) {
    *e = MarketEvent{};
    e->type = MarketEvent::Type::TradingAction;
    e->state = state;
    int n = 0;
    for (; n < 4 && reason[n] && reason[n] != ' '; ++n) e->reason[n] = reason[n];
    e->reason[n] = '\0';
    e->sym = sym;
    e->ts_ns = ts;
    self->events_.publish();
    if (self->waker_) self->waker_->notify();
  }
}

void MarketRunner::write_baseline() {
  if (baseline_written_ || opts_.baseline_dir.empty() || !opts_.write_baseline) return;
  BaselineStore store(opts_.baseline_dir);
  const std::size_t rows = store.append_session(date_, board_, symbols_);
  std::string rep;
  if (rows) store.rebuild(20, &rep);
  baseline_written_ = true;
  LOG_INFO("baseline: session %s appended (%zu symbols); %s", date_.c_str(), rows, rep.c_str());
}

void MarketRunner::run(const std::atomic<bool>& stop) {
  if (file_) {
    while (!stop.load(std::memory_order_relaxed)) {
      if (!file_->run(*book_, stop, opts_.replay_speed, 1 << 16)) {
        file_done_ = true;
        break;
      }
      book_->update_peak();
    }
    if (file_done_) {
      LOG_INFO("itch replay finished: %llu messages", static_cast<unsigned long long>(file_->stats().messages));
      write_baseline();
    }
    return;
  }
  if (mold_) {
    uint64_t iter = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      mold_->poll(*book_, 10);
      if (book_->last_system_event() == 'C' && !baseline_written_) write_baseline();
      if ((++iter & 0xFFFF) == 0) book_->update_peak();
    }
    return;
  }
  if (bridge_) {
    while (!stop.load(std::memory_order_relaxed)) bridge_->poll(symbols_, board_, 50, &MarketRunner::on_bridge_state, this);
    // Bridge feeds have no end-of-day message: write the baseline if we are
    // shutting down after the close.
    if (timeutil::eastern_minute_of_day(static_cast<int64_t>(wall_ns())) >= 16 * 60 + 5) write_baseline();
    return;
  }
  while (!stop.load(std::memory_order_relaxed)) std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

std::string MarketRunner::stats_report() const {
  char buf[512];
  const auto& b = book_ ? book_->stats() : ItchBook::Stats{};
  std::snprintf(buf, sizeof(buf),
                "market[%s %s]: msgs=%llu adds=%llu execs=%llu trades=%llu crosses=%llu halts=%llu "
                "unknown_ref=%llu live_orders=%zu peak_orders=%llu",
                opts_.source.c_str(), date_.c_str(), (unsigned long long)b.messages, (unsigned long long)b.adds,
                (unsigned long long)b.execs, (unsigned long long)b.trades, (unsigned long long)b.crosses,
                (unsigned long long)b.halts, (unsigned long long)b.unknown_ref, book_ ? book_->live_orders() : 0,
                (unsigned long long)b.max_orders);
  std::string out = buf;
  if (mold_) {
    const auto& m = mold_->stats();
    std::snprintf(buf, sizeof(buf), " | mold packets=%llu msgs=%llu gaps=%llu gap_msgs=%llu dups=%llu bad=%llu",
                  (unsigned long long)m.packets, (unsigned long long)m.messages, (unsigned long long)m.gaps,
                  (unsigned long long)m.gap_messages, (unsigned long long)m.dups, (unsigned long long)m.bad);
    out += buf;
  }
  if (bridge_) {
    const auto& s = bridge_->stats();
    std::snprintf(buf, sizeof(buf), " | bridge datagrams=%llu trades=%llu states=%llu bad=%llu unknown=%llu",
                  (unsigned long long)s.datagrams, (unsigned long long)s.trades, (unsigned long long)s.states,
                  (unsigned long long)s.bad, (unsigned long long)s.unknown_symbol);
    out += buf;
  }
  return out;
}

}  // namespace stok
