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

#include <quoll/async/detail/report.hpp>
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
#include <utility>

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

// Made on first use, so that a report from another source's static initialization finds it built.
[[nodiscard]] detail::synchronized<budget> &budget_in_use() {
  static detail::synchronized<budget> in_use;

  return in_use;
}

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

// What report() counts its seconds on: a test's, or steady_clock's.
std::atomic<detail::report_clock> clock_in_use{nullptr};

[[nodiscard]] std::chrono::steady_clock::time_point now() noexcept {
  auto *const clock = clock_in_use.load(std::memory_order_acquire);

  return clock != nullptr ? clock() : std::chrono::steady_clock::now();
}

void write(std::string_view line) noexcept {
  // One call for the whole line, which another thread's does not cut.
  if (auto *const sink = sink_in_use.load(std::memory_order_acquire)) {
    sink(line);
  } else {
    static_cast<void>(std::fwrite(line.data(), 1, line.size(), stderr));
  }
}

void write_left_out(std::size_t left_out) {
  if (left_out > 0) {
    write("quoll: " + std::to_string(left_out) + " more reports left out since the last line written\n");
  }
}

void write_report(std::string_view what, std::exception_ptr const &failure) {
  std::string line{"quoll: "};

  line += what;

  if (failure) {
    line += ": ";
    line += message_of(failure);
  }

  line += '\n';
  write(line);
}

// Out of memory for the line: what it was about goes without its message.
void write_without_memory() noexcept { write("quoll: a failure that could not be reported whole\n"); }

} // namespace

void report(std::string_view what, std::exception_ptr const &failure) noexcept {
  try {
    auto const at = now();
    // How many were left out in a second now over; none if this one goes over the limit.
    auto const before = budget_in_use().with([at](budget &current) -> std::optional<std::size_t> {
      if (at - current.second >= std::chrono::seconds{1}) {
        auto const left_out = current.left_out;

        current = budget{.second = at, .written = 1, .left_out = 0};
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

    write_left_out(*before);
    write_report(what, failure);
  } catch (...) {
    write_without_memory();
  }
}

report_sink set_report_sink(report_sink sink) noexcept {
  // A sink of its own counts its lines afresh, the one it replaces told first of those it left out.
  auto const left_out = budget_in_use().with([](budget &current) { return std::exchange(current, budget{}).left_out; });

  try {
    write_left_out(left_out);
  } catch (...) {
    write_without_memory();
  }

  return sink_in_use.exchange(sink, std::memory_order_acq_rel);
}

namespace detail {

void report_always(std::string_view what, std::exception_ptr const &failure) noexcept {
  try {
    auto const left_out = budget_in_use().with([](budget &current) { return std::exchange(current.left_out, 0); });

    write_left_out(left_out);
    write_report(what, failure);
  } catch (...) {
    write_without_memory();
  }
}

report_clock set_report_clock(report_clock clock) noexcept {
  return clock_in_use.exchange(clock, std::memory_order_acq_rel);
}

} // namespace detail

} // namespace quoll::async
