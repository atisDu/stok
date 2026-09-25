#include "app/settings.hpp"

#include <cstdlib>

#include "core/ascii.hpp"
#include "util/file.hpp"

namespace stok {

namespace {

std::string resolve_secret(const std::string& v) {
  if (istarts_with(v, "env:")) {
    const char* e = std::getenv(v.substr(4).c_str());
    return e ? e : "";
  }
  return v;
}

Tier parse_tier(const std::string& s, Tier def) {
  if (iequals(s, "watch")) return Tier::Watch;
  if (iequals(s, "alert")) return Tier::Alert;
  if (iequals(s, "high")) return Tier::High;
  return def;
}

}  // namespace

std::vector<std::string> feed_names(const Settings& s) {
  std::vector<std::string> n;
  for (const auto& f : s.feeds) n.push_back(f.name);
  return n;
}

bool load_settings(const Config& cfg, Settings& s, std::string* err) {
  // ---- general ----
  const auto& g = cfg.section("general");
  s.data_dir = g.get_str("data_dir", s.data_dir);
  s.ref_dir = g.get_str("ref_dir", fileutil::join(s.data_dir, "ref"));
  s.journal_dir = g.get_str("journal_dir", fileutil::join(s.data_dir, "journal"));
  s.baseline_dir = g.get_str("baseline_dir", fileutil::join(s.data_dir, "baseline"));
  s.rules_file = g.get_str("rules_file", s.rules_file);
  s.log_file = g.get_str("log_file", "");
  const std::string lvl = to_lower_copy(g.get_str("log_level", "info"));
  s.log_level = lvl == "debug" ? LogLevel::Debug : lvl == "warn" ? LogLevel::Warn : lvl == "error" ? LogLevel::Error
                                                                                                    : LogLevel::Info;

  // ---- threads ----
  const auto& t = cfg.section("threads");
  s.threads.net_cpu = static_cast<int>(t.get_int("net_cpu", -1));
  s.threads.engine_cpu = static_cast<int>(t.get_int("engine_cpu", -1));
  s.threads.market_cpu = static_cast<int>(t.get_int("market_cpu", -1));
  s.threads.rt_priority = static_cast<int>(t.get_int("rt_priority", 0));
  s.threads.mlock = t.get_bool("mlock", false);

  // ---- net ----
  const auto& n = cfg.section("net");
  s.poller.user_agent = n.get_str("user_agent", s.poller.user_agent);
  s.poller.ca_file = n.get_str("ca_file", "");
  s.poller.verify_tls = n.get_bool("verify_tls", true);
  s.poller.busy_poll = n.get_bool("busy_poll", false);
  s.poller.connect_timeout_ms = static_cast<int>(n.get_int("connect_timeout_ms", 3000));
  s.poller.io_timeout_ms = static_cast<int>(n.get_int("io_timeout_ms", 8000));
  s.poller.default_max_rps = n.get_double("default_max_rps", 5.0);
  s.poller.fetch_exhibits = n.get_bool("fetch_exhibits", true);
  s.poller.exhibit_max_shares = n.get_double("exhibit_max_shares", 500e6);
  s.poller.stats_interval_s = static_cast<int>(n.get_int("stats_interval_s", 60));

  for (const auto* h : cfg.sections("host")) {
    if (h->arg.empty()) continue;
    s.hosts.push_back(HostPolicy{h->arg, h->get_double("max_rps", s.poller.default_max_rps)});
  }
  for (const auto* f : cfg.sections("feed")) {
    if (f->arg.empty() || !f->get_bool("enabled", true)) continue;
    FeedSpec fs;
    fs.name = f->arg;
    fs.url = f->get_str("url", "");
    if (fs.url.empty()) {
      if (err) *err = "feed " + fs.name + " has no url";
      return false;
    }
    bool ok = true;
    fs.kind = parse_feed_kind(f->get_str("kind", "rss"), &ok);
    if (!ok) {
      if (err) *err = "feed " + fs.name + ": unknown kind (rss|edgar|halts)";
      return false;
    }
    fs.interval_ms = static_cast<uint32_t>(f->get_int("interval_ms", 1000));
    fs.lanes = static_cast<uint32_t>(f->get_int("lanes", 1));
    fs.emit_backlog = f->get_bool("emit_backlog", false);
    // headers = Name: value | Other: value
    split(f->get_str("headers", ""), '|', [&](std::string_view h) {
      h = trim(h);
      if (!h.empty()) fs.extra_headers += std::string(h) + "\r\n";
    });
    s.feeds.push_back(std::move(fs));
  }
  if (s.feeds.size() > 60000) {
    if (err) *err = "too many feeds";
    return false;
  }

  // ---- universe / signal / movers / dilution ----
  const auto& u = cfg.section("universe");
  EngineConfig& e = s.engine;
  e.min_price = u.get_double("min_price", e.min_price);
  e.max_price = u.get_double("max_price", e.max_price);
  e.max_market_cap = u.get_double("max_market_cap", e.max_market_cap);
  e.listed_only = u.get_bool("listed_only", e.listed_only);
  e.allow_unknown_price = u.get_bool("allow_unknown_price", e.allow_unknown_price);

  const auto& sg = cfg.section("signal");
  e.watch_score = static_cast<int>(sg.get_int("watch_score", e.watch_score));
  e.high_score = static_cast<int>(sg.get_int("high_score", e.high_score));
  e.alert_move_pct = sg.get_double("alert_move_pct", e.alert_move_pct);
  e.alert_rvol = sg.get_double("alert_rvol", e.alert_rvol);
  e.alert_dollar_volume = sg.get_double("alert_dollar_volume", e.alert_dollar_volume);
  e.high_max_shares = sg.get_double("high_max_shares", e.high_max_shares);
  e.high_require_above_vwap = sg.get_bool("high_require_above_vwap", e.high_require_above_vwap);
  e.watch_window_s = static_cast<int>(sg.get_int("watch_window_min", e.watch_window_s / 60) * 60);
  e.cooldown_s = static_cast<int>(sg.get_int("cooldown_min", e.cooldown_s / 60) * 60);
  e.push_watch = sg.get_bool("push_watch", e.push_watch);
  const std::string ha = to_lower_copy(sg.get_str("halt_alerts", "watchlist"));
  e.halt_alerts = ha == "none" ? 0 : ha == "universe" ? 2 : 1;
  e.eval_interval_ns = static_cast<uint64_t>(sg.get_int("eval_interval_us", 2000)) * 1000;
  e.busy_poll = sg.get_bool("busy_poll", false);
  e.stats_interval_s = static_cast<int>(sg.get_int("stats_interval_s", 60));
  if (auto hz = sg.get_list("outcome_horizons_s"); !hz.empty()) {
    e.outcome_horizons_s.clear();
    for (const auto& h : hz)
      if (auto v = parse_int<int>(h)) e.outcome_horizons_s.push_back(*v);
  }

  const auto& mv = cfg.section("movers");
  e.movers_enabled = mv.get_bool("enabled", e.movers_enabled);
  e.mover_move_pct = mv.get_double("move_pct", e.mover_move_pct);
  e.mover_rvol = mv.get_double("rvol", e.mover_rvol);
  e.mover_dollar_volume = mv.get_double("dollar_volume", e.mover_dollar_volume);
  e.mover_scan_ms = static_cast<int>(mv.get_int("scan_ms", e.mover_scan_ms));

  const auto& d = cfg.section("dilution");
  e.dilution.offering_window_days = static_cast<int>(d.get_int("offering_window_days", e.dilution.offering_window_days));
  e.dilution.s1_window_days = static_cast<int>(d.get_int("s1_window_days", e.dilution.s1_window_days));
  e.dilution.shelf_window_days = static_cast<int>(d.get_int("shelf_window_days", e.dilution.shelf_window_days));
  e.dilution.proxy_window_days = static_cast<int>(d.get_int("proxy_window_days", e.dilution.proxy_window_days));

  // ---- market ----
  const auto& m = cfg.section("market");
  MarketOptions& mo = s.market;
  mo.source = to_lower_copy(m.get_str("source", "none"));
  mo.itch_file = m.get_str("itch_file", "");
  mo.replay_speed = m.get_double("replay_speed", 1.0);
  mo.session_date = m.get_str("session_date", "");
  mo.mold.group = m.get_str("mold_group", "");
  mo.mold.port = static_cast<uint16_t>(m.get_int("mold_port", 0));
  mo.mold.iface_addr = m.get_str("mold_iface", "");
  mo.mold.rcvbuf_bytes = static_cast<int>(m.get_int("rcvbuf_bytes", 64 << 20));
  mo.bridge_bind = m.get_str("bridge_bind", "127.0.0.1");
  mo.bridge_port = static_cast<uint16_t>(m.get_int("bridge_port", 7777));
  mo.order_capacity = static_cast<std::size_t>(m.get_int("order_capacity", 1 << 22));
  mo.write_baseline = m.get_bool("write_baseline", true);
  mo.busy_poll = m.get_bool("busy_poll", false);
  mo.baseline_dir = s.baseline_dir;

  // ---- alerts ----
  const auto& a = cfg.section("alerts");
  AlertOptions& ao = s.alerts;
  ao.stdout_enabled = a.get_bool("stdout", true);
  ao.telegram_token = resolve_secret(a.get_str("telegram_token", ""));
  ao.telegram_chat_id = resolve_secret(a.get_str("telegram_chat_id", ""));
  if (const char* v = std::getenv("STOK_TELEGRAM_TOKEN"); v && *v) ao.telegram_token = v;
  if (const char* v = std::getenv("STOK_TELEGRAM_CHAT_ID"); v && *v) ao.telegram_chat_id = v;
  ao.telegram_min_tier = parse_tier(a.get_str("telegram_min_tier", "alert"), Tier::Alert);
  ao.telegram_halts = a.get_bool("telegram_halts", true);
  ao.telegram_movers = a.get_bool("telegram_movers", true);
  ao.udp_target = a.get_str("udp_target", "");
  ao.keepalive_ping_s = static_cast<int>(a.get_int("keepalive_ping_s", 25));
  ao.user_agent = s.poller.user_agent;
  ao.ca_file = s.poller.ca_file;
  ao.verify_tls = s.poller.verify_tls;
  if (!ao.telegram_token.empty() && ao.telegram_chat_id.empty()) {
    if (err) *err = "alerts.telegram_token is set but telegram_chat_id is empty";
    return false;
  }

  const auto& q = cfg.section("queues");
  s.news_queue = static_cast<std::size_t>(q.get_int("news", static_cast<int64_t>(s.news_queue)));
  s.market_event_queue = static_cast<std::size_t>(q.get_int("market_events", static_cast<int64_t>(s.market_event_queue)));
  s.alert_queue = static_cast<std::size_t>(q.get_int("alerts", static_cast<int64_t>(s.alert_queue)));
  s.journal_queue = static_cast<std::size_t>(q.get_int("journal", static_cast<int64_t>(s.journal_queue)));
  return true;
}

}  // namespace stok
