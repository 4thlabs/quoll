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

#include <quoll/async.hpp>

#include <catch2/catch_test_macros.hpp>

#include <coroutine>
#include <vector>

namespace {

using quoll::async::executor_ref;
using quoll::async::run_loop;

// A third-party style executor: no base class, only the Executor requirements.
class recording_executor {
public:
  explicit recording_executor(std::vector<std::coroutine_handle<>> &posted) noexcept : posted_(&posted) {}

  void post(quoll::async::work_item &item) const { posted_->push_back(item.handle); }

  [[nodiscard]] bool operator==(recording_executor const &) const noexcept = default;

private:
  std::vector<std::coroutine_handle<>> *posted_;
};

static_assert(quoll::async::Executor<recording_executor>);

} // namespace

TEST_CASE("executor_ref posts to the referenced executor", "[async][executor_ref]") {
  run_loop loop;
  auto const executor = loop.get_executor();
  executor_ref const ref{executor};
  quoll::async::work_item item{std::noop_coroutine()};

  ref.post(item);

  REQUIRE(loop.run() == 1);
}

TEST_CASE("executor_ref wraps any Executor without intrusion", "[async][executor_ref]") {
  std::vector<std::coroutine_handle<>> posted;
  recording_executor const executor{posted};
  executor_ref const ref{executor};
  quoll::async::work_item item{std::noop_coroutine()};

  ref.post(item);

  REQUIRE(posted.size() == 1);
  REQUIRE(posted.front() == item.handle);
}

TEST_CASE("executor_ref recovers the concrete executor", "[async][executor_ref]") {
  run_loop loop;
  auto const executor = loop.get_executor();
  executor_ref const ref{executor};

  REQUIRE(ref.target<run_loop::executor>() == &executor);
  REQUIRE(ref.target<recording_executor>() == nullptr);
}

TEST_CASE("executor_ref compares the referenced executors", "[async][executor_ref]") {
  run_loop loop;
  run_loop other_loop;
  std::vector<std::coroutine_handle<>> posted;
  auto const executor = loop.get_executor();
  auto const same_loop = loop.get_executor();
  auto const other = other_loop.get_executor();
  recording_executor const recording{posted};

  REQUIRE(executor_ref{executor} == executor_ref{same_loop});
  REQUIRE_FALSE(executor_ref{executor} == executor_ref{other});
  REQUIRE_FALSE(executor_ref{executor} == executor_ref{recording});
}
