#include <csignal>
#include <iostream>
#include <string>
#include <vector>

#include "cli.hpp"
#include "exp.hpp"
#include "log_tee.hpp"
#include "nobion/core/clock.hpp"
#include "nobion/core/log_context.hpp"
#include "nobion/core/log_dir.hpp"
#include "nobion/core/log_hub.hpp"
#include "nobion/core/process_log.hpp"
#include "process_log.hpp"

extern "C" void elctl_on_interrupt(int) { ++elctl::interrupt_count(); }

int main(int argc, char** argv) {
  const nobion::LogContext::Thread log_thread("elctl");
  nobion::LogHub::install_crash_handlers();
  std::signal(SIGINT, elctl_on_interrupt);
  std::vector<std::string> args(argv + 1, argv + argc);
  // One hub for the process: every command logs to elctl.log. It outlives run()
  // and goes before the clock it was given.
  const nobion::SteadyClock clock;
  nobion::set_process_log_hub(
      elctl::open_process_log(args, clock, nobion::host_os(), nobion::process_env(), std::cerr));
  int code = 0;
  {
    // What elctl tells the user on stderr is also in its log (an `error...`
    // line at error, any other at warn). std::cerr has its own buffer back
    // before the hub is unset.
    const elctl::StreamTee tee(std::cerr, nobion::process_logger("elctl"));
    code = elctl::run(args, elctl::Io{std::cin, std::cout, std::cerr});
  }
  nobion::set_process_log_hub(nullptr);
  return code;
}
