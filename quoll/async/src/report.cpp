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

#include <atomic>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>

namespace quoll::async::detail {

namespace {

// What a test replaced stderr with; read on whichever thread reports.
std::atomic<report_sink> sink_in_use{nullptr};

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

} // namespace

void report(std::string_view what, std::exception_ptr const &failure) noexcept {
  try {
    std::string line{"quoll: "};

    line += what;
    line += ": ";
    line += message_of(failure);
    line += '\n';

    // One call for the whole line, which another thread's does not cut.
    if (auto *const sink = sink_in_use.load(std::memory_order_acquire)) {
      sink(line);
    } else {
      std::fputs(line.c_str(), stderr);
    }
  } catch (...) {
    // Out of memory for the line: what it was about goes without its message.
    std::fputs("quoll: a failure that could not be reported whole\n", stderr);
  }
}

report_sink set_report_sink(report_sink sink) noexcept { return sink_in_use.exchange(sink, std::memory_order_acq_rel); }

} // namespace quoll::async::detail
