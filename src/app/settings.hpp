#pragma once

#include <string>
#include <vector>

#include "core/config.hpp"
#include "core/log.hpp"
#include "engine/engine.hpp"
#include "feeds/poller.hpp"
#include "market/runner.hpp"
#include "sink/alerts.hpp"

namespace stok {

struct Settings {
  std::string data_dir = "data";
  std::string ref_dir;       // default <data_dir>/ref
  std::string journal_dir;   // default <data_dir>/journal
  std::string baseline_dir;  // default <data_dir>/baseline
  std::string rules_file = "config/rules.tsv";
  std::string log_file;
  LogLevel log_level = LogLevel::Info;

  struct Threads {
    int net_cpu = -1;
    int engine_cpu = -1;
    int market_cpu = -1;
    int rt_priority = 0;  // >0: SCHED_FIFO for the pinned threads
    bool mlock = false;
  } threads;

  std::size_t news_queue = 4096;
  std::size_t market_event_queue = 16384;
  std::size_t alert_queue = 1024;
  std::size_t journal_queue = 4096;

  PollerOptions poller;
  std::vector<FeedSpec> feeds;
  std::vector<HostPolicy> hosts;
  EngineConfig engine;
  MarketOptions market;
  AlertOptions alerts;
};

// Maps the INI config onto the option structs. Secrets may come from the
// environment: STOK_TELEGRAM_TOKEN, STOK_TELEGRAM_CHAT_ID, or "env:VAR" values.
bool load_settings(const Config& cfg, Settings& out, std::string* err);

std::vector<std::string> feed_names(const Settings& s);

}  // namespace stok
