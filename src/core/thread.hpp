#pragma once

#include <string>

namespace stok {

// Pins the calling thread to one CPU. Returns false (and leaves the thread
// unpinned) if cpu < 0 or the call fails.
bool pin_current_thread(int cpu);

// Sets the calling thread's name (visible in top/htop/perf). Max 15 chars.
void set_current_thread_name(const std::string& name);

// Asks for SCHED_FIFO at `priority`. Needs CAP_SYS_NICE; returns false if
// refused.
bool set_realtime_priority(int priority);

// mlockall(MCL_CURRENT | MCL_FUTURE): no page faults on hot paths after
// warm-up. Needs RLIMIT_MEMLOCK or CAP_IPC_LOCK.
bool lock_all_memory();

}  // namespace stok
