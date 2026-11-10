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
#include <quoll/test/reports.hpp>

#include <quoll/async/detail/synchronized.hpp>

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace quoll::test {

namespace {

[[nodiscard]] async::detail::synchronized<std::vector<std::string>> &report_store() {
  static async::detail::synchronized<std::vector<std::string>> store;

  return store;
}

} // namespace

void keep_report(std::string_view line) noexcept {
  try {
    report_store().with([&](std::vector<std::string> &kept) { kept.emplace_back(line); });
  } catch (...) {
    // A test that runs out of memory reports nothing more.
  }
}

std::vector<std::string> reports() {
  return report_store().with([](std::vector<std::string> const &kept) { return kept; });
}

void clear_reports() {
  report_store().with([](std::vector<std::string> &kept) { kept.clear(); });
}

} // namespace quoll::test

namespace {

// For every test case of the binary: what is reported goes to quoll::test::reports(), not to stderr, which the tests
// that throw on purpose would fill.
class keeping_reports : public Catch::EventListenerBase {
public:
  using Catch::EventListenerBase::EventListenerBase;

  // Set again for each run of a case, a section's or a generator's, so that each has its own ten lines a second.
  void testCasePartialStarting(Catch::TestCaseInfo const &, std::uint64_t) override {
    // The sink first: setting it tells the one it replaces of what the run before left out, here the same.
    quoll::async::set_report_sink(&quoll::test::keep_report);
    quoll::test::clear_reports();
  }
};

} // namespace

CATCH_REGISTER_LISTENER(keeping_reports)
