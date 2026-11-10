//   ___              _ _
//  / _ \ _   _  ___ | | |
// | | | | | | |/ _ \| | |
// | |_| | |_| | (_) | | |
//  \__\_\\__,_|\___/|_|_|
//
// BSD 3-Clause License
//
// Copyright (c) 2026, Thomas Gourgues (thomas.gourgues@gmail.com)
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <quoll/async/report.hpp>

#include <quoll/async/detail/synchronized.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

namespace quoll::async {

namespace {

// What a program replaced stderr with; read on whichever thread reports.
std::atomic<report_sink> sink_in_use{nullptr};

// The lines of the current second, and those left out past the limit.
struct budget {
  std::chrono::steady_clock::time_point second{};
  std::size_t written = 0;
  std::size_t left_out = 0;
};

constexpr std::size_t lines_a_second = 10;

detail::synchronized<budget> budget_in_use;

[[nodiscard]] std::string message_of(std::exception_ptr const &failure) {
  if (!failure) {
    return "no exception";
  }

  try {
    std::rethrow_exception(failure);
  } catch (std::exception const &thrown) {
    return thrown.what();
  } catch (...) {
    return "an exception of a type that is no std::exception";
  }
}

void write(std::string const &line) noexcept {
  // One call for the whole line, which another thread's does not cut.
  if (auto *const sink = sink_in_use.load(std::memory_order_acquire)) {
    sink(line);
  } else {
    std::fputs(line.c_str(), stderr);
  }
}

} // namespace

void report(std::string_view what, std::exception_ptr const &failure) noexcept {
  try {
    auto const now = std::chrono::steady_clock::now();
    // How many went before this one, in a second now over; none if this one goes over the limit.
    auto const before = budget_in_use.with([now](budget &current) -> std::optional<std::size_t> {
      if (now - current.second >= std::chrono::seconds{1}) {
        auto const left_out = current.left_out;

        current = budget{.second = now, .written = 1, .left_out = 0};
        return left_out;
      }

      if (current.written >= lines_a_second) {
        ++current.left_out;
        return std::nullopt;
      }

      ++current.written;
      return 0;
    });

    if (!before) {
      return;
    }

    if (*before > 0) {
      write("quoll: " + std::to_string(*before) + " more reports left out in the second before\n");
    }

    std::string line{"quoll: "};

    line += what;
    line += ": ";
    line += message_of(failure);
    line += '\n';
    write(line);
  } catch (...) {
    // Out of memory for the line: what it was about goes without its message.
    std::fputs("quoll: a failure that could not be reported whole\n", stderr);
  }
}

report_sink set_report_sink(report_sink sink) noexcept {
  // A sink of its own counts its lines afresh.
  budget_in_use.with([](budget &current) { current = budget{}; });
  return sink_in_use.exchange(sink, std::memory_order_acq_rel);
}

} // namespace quoll::async
