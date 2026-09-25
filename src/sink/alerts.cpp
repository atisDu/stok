#include "sink/alerts.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include "core/ascii.hpp"
#include "core/clock.hpp"
#include "core/log.hpp"
#include "ref/filings.hpp"
#include "util/text.hpp"
#include "util/time.hpp"

namespace stok {

namespace {

const char* tier_icon(Tier t) {
  switch (t) {
    case Tier::Watch: return "\xF0\x9F\x9F\xA1";   // yellow circle
    case Tier::Alert: return "\xF0\x9F\x9F\xA0";   // orange circle
    case Tier::High: return "\xF0\x9F\x94\xB4";    // red circle
    case Tier::Halt: return "\xE2\x8F\xB8";        // pause
    case Tier::Mover: return "\xF0\x9F\x9A\x80";   // rocket
    case Tier::Info: return "\xE2\x9A\xA0";        // warning
    default: return "";
  }
}

std::string money(double v) {
  char b[32];
  if (v >= 1e9) std::snprintf(b, sizeof(b), "$%.2fB", v / 1e9);
  else if (v >= 1e6) std::snprintf(b, sizeof(b), "$%.1fM", v / 1e6);
  else if (v >= 1e3) std::snprintf(b, sizeof(b), "$%.0fK", v / 1e3);
  else std::snprintf(b, sizeof(b), "$%.0f", v);
  return b;
}

std::string shares(double v) {
  char b[32];
  if (v >= 1e9) std::snprintf(b, sizeof(b), "%.2fB", v / 1e9);
  else if (v >= 1e6) std::snprintf(b, sizeof(b), "%.1fM", v / 1e6);
  else std::snprintf(b, sizeof(b), "%.0fK", v / 1e3);
  return b;
}

void html_escape(std::string& out, std::string_view s) {
  for (char c : s) {
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else out.push_back(c);
  }
}

}  // namespace

AlertSink::AlertSink(AlertOptions opts, std::vector<std::string> source_names)
    : opts_(std::move(opts)), sources_(std::move(source_names)) {}

AlertSink::~AlertSink() {
  if (udp_fd_ >= 0) ::close(udp_fd_);
}

bool AlertSink::init(std::string* err) {
  if (!opts_.telegram_token.empty()) {
    tls_ = std::make_unique<net::TlsContext>(opts_.ca_file, opts_.verify_tls);
    if (!tls_->ok()) {
      if (err) *err = tls_->error();
      return false;
    }
    resolver_ = std::make_unique<net::Resolver>();
    http_ = std::make_unique<net::HttpClient>(*tls_, *resolver_, opts_.user_agent);
    auto r = http_->get("https://api.telegram.org/bot" + opts_.telegram_token + "/getMe", 8000);
    if (!r.ok()) {
      LOG_WARN("telegram: getMe failed (status %d %s); alerts will retry", r.status, r.error.c_str());
    } else {
      LOG_INFO("telegram: connected (%.1f ms handshake+request)", static_cast<double>(r.total_ns) / 1e6);
    }
    last_telegram_io_ = mono_ns();
  }
  if (!opts_.udp_target.empty()) {
    const auto colon = opts_.udp_target.rfind(':');
    if (colon == std::string::npos) {
      if (err) *err = "alerts.udp_target must be host:port";
      return false;
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(std::stoi(opts_.udp_target.substr(colon + 1))));
    if (inet_pton(AF_INET, opts_.udp_target.substr(0, colon).c_str(), &a.sin_addr) != 1) {
      if (err) *err = "alerts.udp_target: bad IPv4 address";
      return false;
    }
    udp_fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    udp_addr_.assign(reinterpret_cast<unsigned char*>(&a), reinterpret_cast<unsigned char*>(&a) + sizeof(a));
  }
  return true;
}

std::string AlertSink::format_text(const Signal& s, const std::vector<std::string>& sources) {
  char line[512];
  std::string out;
  out.reserve(700);
  std::snprintf(line, sizeof(line), "%s %s %s", tier_icon(s.tier), tier_name(s.tier), s.ticker);
  out += line;
  if (s.price > 0) {
    std::snprintf(line, sizeof(line), s.price < 1.0 ? "  $%.4f" : "  $%.2f", s.price);
    out += line;
    if (s.ref_price > 0 && s.tier != Tier::Watch) {
      std::snprintf(line, sizeof(line), " (%+.1f%% %s)", s.move_pct, s.tier == Tier::Mover ? "vs prev close" : "since news");
      out += line;
    }
  }
  if (s.rvol > 0) {
    std::snprintf(line, sizeof(line), "  RVOL %.1fx", s.rvol);
    out += line;
  }
  if (s.dollar_volume > 0) out += "  " + money(s.dollar_volume) + " traded";
  if (s.spread_pct > 0) {
    std::snprintf(line, sizeof(line), "  spread %.1f%%", s.spread_pct);
    out += line;
  }
  if (s.halt_reason[0]) {
    out += "  [";
    out += s.halt_reason;
    out += "]";
  }
  out += '\n';
  if (!s.why.empty()) {
    out.append(s.why.view());
    out += '\n';
  }
  std::string ctx;
  if (s.shares_out > 0) ctx += "shares " + shares(s.shares_out);
  if (s.market_cap > 0) ctx += (ctx.empty() ? "" : " · ") + std::string("mcap ") + money(s.market_cap);
  if (s.exchange[0]) ctx += (ctx.empty() ? "" : " · ") + std::string(s.exchange);
  ctx += (ctx.empty() ? "" : " · ") + std::string("filings: ") +
         (s.dilution_flags ? dilution_flags_str(s.dilution_flags) : std::string("clean"));
  if (s.score_flags & (kSfNonBinding | kSfUpTo | kSfPromo)) ctx += " · text: " + score_flags_str(s.score_flags & (kSfNonBinding | kSfUpTo | kSfPromo));
  out += ctx + '\n';
  if (!s.title.empty()) {
    out += '"';
    out.append(s.title.view());
    out += "\"\n";
  }
  const char* src = s.source < sources.size() ? sources[s.source].c_str() : "?";
  std::snprintf(line, sizeof(line), "score %d · %s · src %s", s.score, catalyst_name(s.catalyst), src);
  out += line;
  if (s.t_news_recv_ns && s.t_signal_ns > s.t_news_recv_ns && s.tier != Tier::Watch && s.tier != Tier::Mover) {
    std::snprintf(line, sizeof(line), " · signal %.1fs after news", static_cast<double>(s.t_signal_ns - s.t_news_recv_ns) / 1e9);
    out += line;
  }
  out += " · ";
  out += timeutil::eastern_hms(static_cast<int64_t>(s.t_signal_ns));
  out += " ET";
  if (!s.link.empty()) {
    out += '\n';
    out.append(s.link.view());
  }
  return out;
}

std::string AlertSink::format_html(const Signal& s, const std::vector<std::string>& sources) {
  // Telegram HTML: escape the plain text, bold the header line.
  const std::string plain = format_text(s, sources);
  const auto nl = plain.find('\n');
  std::string out = "<b>";
  html_escape(out, std::string_view(plain).substr(0, nl));
  out += "</b>";
  if (nl != std::string::npos) html_escape(out, std::string_view(plain).substr(nl));
  return out;
}

bool AlertSink::send_telegram(const Signal& s) {
  if (!http_) return false;
  const uint64_t now = mono_ns();
  if (now < telegram_backoff_until_) return false;
  std::string body = "{\"chat_id\":\"";
  text::append_json_escaped(body, opts_.telegram_chat_id);
  body += "\",\"parse_mode\":\"HTML\",\"disable_web_page_preview\":true,\"text\":\"";
  text::append_json_escaped(body, format_html(s, sources_));
  body += "\"}";
  auto r = http_->post("https://api.telegram.org/bot" + opts_.telegram_token + "/sendMessage", body, "application/json",
                       8000);
  last_telegram_io_ = mono_ns();
  if (r.status == 429) {
    // {"ok":false,"error_code":429,"parameters":{"retry_after":N}}
    int retry = 3;
    const auto p = r.body.find("retry_after");
    if (p != std::string::npos) {
      std::size_t k = p + 11;
      while (k < r.body.size() && !is_digit(r.body[k])) ++k;
      retry = std::atoi(r.body.c_str() + k);
    }
    telegram_backoff_until_ = mono_ns() + static_cast<uint64_t>(std::max(1, retry)) * kNsPerSec;
    LOG_WARN("telegram: rate limited, retry after %ds", retry);
    return false;
  }
  if (!r.ok()) {
    LOG_WARN("telegram: send failed (status %d %s)", r.status, r.error.c_str());
    return false;
  }
  return true;
}

void AlertSink::keepalive() {
  if (!http_ || opts_.keepalive_ping_s <= 0) return;
  const uint64_t now = mono_ns();
  if (now - last_telegram_io_ < static_cast<uint64_t>(opts_.keepalive_ping_s) * kNsPerSec) return;
  http_->get("https://api.telegram.org/bot" + opts_.telegram_token + "/getMe", 5000);
  last_telegram_io_ = mono_ns();
}

void AlertSink::send_udp(const Signal& s) {
  if (udp_fd_ < 0) return;
  std::string j = "{\"tier\":\"";
  j += tier_name(s.tier);
  j += "\",\"ticker\":\"";
  text::append_json_escaped(j, s.ticker);
  char b[256];
  std::snprintf(b, sizeof(b), "\",\"price\":%.4f,\"move_pct\":%.2f,\"rvol\":%.2f,\"score\":%d,\"signal_ns\":%llu,\"title\":\"",
                s.price, s.move_pct, s.rvol, s.score, static_cast<unsigned long long>(s.t_signal_ns));
  j += b;
  text::append_json_escaped(j, s.title.view());
  j += "\",\"why\":\"";
  text::append_json_escaped(j, s.why.view());
  j += "\",\"link\":\"";
  text::append_json_escaped(j, s.link.view());
  j += "\"}";
  if (::sendto(udp_fd_, j.data(), j.size(), MSG_DONTWAIT, reinterpret_cast<const sockaddr*>(udp_addr_.data()),
               static_cast<socklen_t>(udp_addr_.size())) > 0)
    ++st_.udp_sent;
}

void AlertSink::deliver(const Signal& s) {
  ++st_.signals;
  send_udp(s);
  if (opts_.stdout_enabled) {
    const std::string t = format_text(s, sources_);
    std::fprintf(stdout, "%s\n\n", t.c_str());
    std::fflush(stdout);
  }
  bool to_telegram = static_cast<int>(s.tier) >= static_cast<int>(opts_.telegram_min_tier);
  if (s.tier == Tier::Halt) to_telegram = opts_.telegram_halts;
  if (s.tier == Tier::Mover) to_telegram = opts_.telegram_movers;
  if (s.tier == Tier::Info) to_telegram = true;
  if (to_telegram && http_) {
    if (send_telegram(s)) {
      ++st_.telegram_sent;
      const uint64_t now = wall_ns();
      if (now > s.t_signal_ns) st_.signal_to_sent.record(now - s.t_signal_ns);
    } else {
      ++st_.telegram_failed;
    }
  }
}

void AlertSink::run(SpscQueue<Signal>& q, Waker& waker, const std::atomic<bool>& stop) {
  Logger::set_thread_tag("alerts");
  while (!stop.load(std::memory_order_relaxed)) {
    bool any = false;
    while (Signal* s = q.front()) {
      deliver(*s);
      q.pop();
      any = true;
    }
    keepalive();
    if (!any) waker.wait(500 * kNsPerMs, [&] { return !q.empty(); });
  }
  while (Signal* s = q.front()) {
    deliver(*s);
    q.pop();
  }
}

}  // namespace stok
