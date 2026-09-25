#include "core/thread.hpp"

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>

namespace stok {

bool pin_current_thread(int cpu) {
  if (cpu < 0) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

void set_current_thread_name(const std::string& name) {
  std::string n = name.substr(0, 15);
  pthread_setname_np(pthread_self(), n.c_str());
}

bool set_realtime_priority(int priority) {
  sched_param sp{};
  sp.sched_priority = priority;
  return pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
}

bool lock_all_memory() { return mlockall(MCL_CURRENT | MCL_FUTURE) == 0; }

}  // namespace stok
