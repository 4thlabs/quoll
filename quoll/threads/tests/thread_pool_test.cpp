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
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <quoll/async/co_spawn.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/test/overloaded.hpp>
#include <quoll/threads.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using quoll::async::task;
using quoll::threads::thread_pool;

using quoll::test::overloaded;

// Runs `work` on the pool. The handler runs on a worker, where Catch2
// assertions are not allowed: the outcome goes through a future, read by the
// test thread.
template <class T> std::future<T> spawn(thread_pool &pool, task<T> work) {
  auto promise = std::make_shared<std::promise<T>>();
  auto future = promise->get_future();

  co_spawn(
      pool.get_executor(), std::move(work),
      overloaded{[promise](T value) { promise->set_value(std::move(value)); },
                 [promise](std::exception_ptr error) {
                   promise->set_exception(std::move(error));
                 }});
  return future;
}

task<std::thread::id> thread_id() { co_return std::this_thread::get_id(); }

task<int> count_yields(int yields) {
  for (int index = 0; index < yields; ++index) {
    co_await quoll::async::yield();
  }

  co_return yields;
}

// Blocks its worker until `expected` tasks have arrived, or a deadline passes:
// they all arrive only if they run on distinct threads at the same time.
task<bool> meet(std::atomic<std::size_t> &arrived, std::size_t expected) {
  arrived.fetch_add(1);

  auto const deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{10};

  while (arrived.load() < expected) {
    if (std::chrono::steady_clock::now() > deadline) {
      co_return false;
    }

    std::this_thread::yield();
  }

  co_return true;
}

task<std::tuple<int, int>> both(int first, int second) {
  co_return co_await quoll::async::when_all(count_yields(first),
                                            count_yields(second));
}

task<int> fail() {
  co_await quoll::async::yield();
  throw std::runtime_error{"boom"};
}

} // namespace

TEST_CASE("thread_pool resumes a spawned task on a worker thread",
          "[threads][thread_pool]") {
  thread_pool pool{2};

  auto const id = spawn(pool, thread_id()).get();

  REQUIRE(id != std::this_thread::get_id());
}

TEST_CASE("thread_pool hands the exception of a task to its handler",
          "[threads][thread_pool]") {
  thread_pool pool{2};

  auto outcome = spawn(pool, fail());

  REQUIRE_THROWS_AS(outcome.get(), std::runtime_error);
}

TEST_CASE("thread_pool resumes coroutines on all its workers at once",
          "[threads][thread_pool]") {
  constexpr std::size_t threads = 4;
  thread_pool pool{threads};
  std::atomic<std::size_t> arrived{0};
  std::vector<std::future<bool>> outcomes;

  for (std::size_t index = 0; index < threads; ++index) {
    outcomes.push_back(spawn(pool, meet(arrived, threads)));
  }

  for (auto &outcome : outcomes) {
    REQUIRE(outcome.get());
  }
}

TEST_CASE("thread_pool runs many tasks posting from every worker",
          "[threads][thread_pool]") {
  thread_pool pool{4};
  std::vector<std::future<int>> outcomes;

  for (int index = 0; index < 200; ++index) {
    outcomes.push_back(spawn(pool, count_yields(index % 20)));
  }

  for (int index = 0; index < 200; ++index) {
    REQUIRE(outcomes[static_cast<std::size_t>(index)].get() == index % 20);
  }
}

TEST_CASE("thread_pool joins when_all children resumed on different workers",
          "[threads][thread_pool]") {
  thread_pool pool{4};

  auto const result = spawn(pool, both(50, 70)).get();

  REQUIRE(result == std::tuple{50, 70});
}

TEST_CASE("thread_pool destruction runs what is still queued",
          "[threads][thread_pool]") {
  std::atomic<int> completed{0};

  {
    thread_pool pool{2};

    for (int index = 0; index < 100; ++index) {
      co_spawn(pool.get_executor(), count_yields(3),
               overloaded{[&](int) { completed.fetch_add(1); },
                          [](std::exception_ptr) {}});
    }
  }

  REQUIRE(completed.load() == 100);
}

TEST_CASE("thread_pool needs at least one thread", "[threads][thread_pool]") {
  REQUIRE_THROWS_AS(thread_pool{0}, std::invalid_argument);
}

TEST_CASE("thread_pool executors are equal when they post to the same pool",
          "[threads][thread_pool]") {
  thread_pool pool{1};
  thread_pool other{1};

  REQUIRE(pool.get_executor() == pool.get_executor());
  REQUIRE_FALSE(pool.get_executor() == other.get_executor());
}
