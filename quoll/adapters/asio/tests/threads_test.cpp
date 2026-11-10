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

// What quoll::async does across threads, on an executor that resumes coroutines on several at once with no strand:
// asio's own thread pool, behind quoll::asio::executor.

#include <quoll/asio/executor.hpp>
#include <quoll/async/await_context.hpp>
#include <quoll/async/co_spawn.hpp>
#include <quoll/async/detail/chain_owner.hpp>
#include <quoll/async/detail/owned_chain.hpp>
#include <quoll/async/detail/slots.hpp>
#include <quoll/async/detail/synchronized.hpp>
#include <quoll/async/stopped.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/test/overloaded.hpp>

#include <catch2/catch_test_macros.hpp>

#include <asio/thread_pool.hpp>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using quoll::async::task;
using quoll::test::overloaded;

template <class T>
std::future<T> spawn(::asio::thread_pool &pool, task<T> work) {
  auto promise = std::make_shared<std::promise<T>>();
  auto future = promise->get_future();

  co_spawn(quoll::asio::executor{pool.get_executor()}, std::move(work),
           overloaded{[promise](T value) { promise->set_value(std::move(value)); },
                      [promise](std::exception_ptr error) { promise->set_exception(std::move(error)); }});
  return future;
}

task<std::thread::id> thread_id() { co_return std::this_thread::get_id(); }

task<int> count_yields(int yields) {
  for (int index = 0; index < yields; ++index) {
    co_await quoll::async::yield();
  }

  co_return yields;
}

task<bool> meet(std::atomic<std::size_t> &arrived, std::size_t expected) {
  arrived.fetch_add(1);

  auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};

  while (arrived.load() < expected) {
    if (std::chrono::steady_clock::now() > deadline) {
      co_return false;
    }

    std::this_thread::yield();
  }

  co_return true;
}

task<std::tuple<int, int>> both(int first, int second) {
  co_return co_await quoll::async::when_all(count_yields(first), count_yields(second));
}

task<int> fail() {
  co_await quoll::async::yield();
  throw std::runtime_error{"boom"};
}

// Threads that end chains on a stop, joined by the test: none lives in a frame, which the stop may destroy at once.
using stopping_threads = quoll::async::detail::synchronized<std::vector<std::jthread>>;

// Ends its awaiter's chain on a stop from a thread of its own, as a stop callback running on another thread would. The
// owner may unwind the chain on a worker while that thread is still in stop(), right after the post. Touches nothing of
// its own once the thread runs.
class stopped_elsewhere {
public:
  explicit stopped_elsewhere(stopping_threads &threads) noexcept : threads_(&threads) {}

  [[nodiscard]] bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<>, quoll::async::await_context const &context) const {
    auto *const owner = context.owner;

    threads_->with([owner](std::vector<std::jthread> &all) {
      all.emplace_back([owner] { quoll::async::detail::stop_chain(owner); });
    });
  }

  void await_resume() const noexcept {}

private:
  stopping_threads *threads_;
};

// Counts its destruction: what a frame destroyed with its locals leaves.
class counted {
public:
  explicit counted(std::atomic<int> &count) noexcept : count_(&count) {}

  counted(counted const &) = delete;
  counted(counted &&) = delete;
  counted &operator=(counted const &) = delete;
  counted &operator=(counted &&) = delete;

  ~counted() { count_->fetch_add(1); }

private:
  std::atomic<int> *count_;
};

task<int> stops_from_another_thread(std::atomic<int> &unwound, stopping_threads &threads) {
  counted const kept{unwound};

  co_await quoll::async::yield();
  co_await stopped_elsewhere{threads};
  co_return 0;
}

} // namespace

TEST_CASE("threads: a spawned task resumes on a worker thread", "[asio][threads]") {
  ::asio::thread_pool pool{2};

  auto const id = spawn(pool, thread_id()).get();

  REQUIRE(id != std::this_thread::get_id());
}

TEST_CASE("threads: co_spawn(this_executor) runs on a worker, past the coroutine that spawned it", "[asio][threads]") {
  std::promise<std::thread::id> spawned;
  ::asio::thread_pool pool{2};

  auto const parent = [&]() -> task<int> {
    co_await quoll::async::co_spawn(quoll::async::this_executor, thread_id(),
                                    overloaded{[&](std::thread::id id) { spawned.set_value(id); },
                                               [&](std::exception_ptr const &error) { spawned.set_exception(error); }});
    co_return 0;
  };

  REQUIRE(spawn(pool, parent()).get() == 0);
  REQUIRE(spawned.get_future().get() != std::this_thread::get_id());
}

TEST_CASE("threads: the exception of a task reaches its handler", "[asio][threads]") {
  ::asio::thread_pool pool{2};

  auto outcome = spawn(pool, fail());

  REQUIRE_THROWS_AS(outcome.get(), std::runtime_error);
}

TEST_CASE("threads: coroutines resume on all the workers at once", "[asio][threads]") {
  constexpr std::size_t threads = 4;
  // Declared before the pool: if a REQUIRE throws, the pool's destructor joins the workers still reading it.
  std::atomic<std::size_t> arrived{0};
  ::asio::thread_pool pool{threads};
  std::vector<std::future<bool>> outcomes;

  for (std::size_t index = 0; index < threads; ++index) {
    outcomes.push_back(spawn(pool, meet(arrived, threads)));
  }

  for (auto &outcome : outcomes) {
    REQUIRE(outcome.get());
  }
}

TEST_CASE("threads: many yielding tasks run to completion", "[asio][threads]") {
  ::asio::thread_pool pool{4};
  std::vector<std::future<int>> outcomes;

  for (int index = 0; index < 200; ++index) {
    outcomes.push_back(spawn(pool, count_yields(index % 20)));
  }

  for (int index = 0; index < 200; ++index) {
    REQUIRE(outcomes[static_cast<std::size_t>(index)].get() == index % 20);
  }
}

TEST_CASE("threads: when_all joins children resumed on different workers", "[asio][threads]") {
  ::asio::thread_pool pool{4};

  auto const result = spawn(pool, both(50, 70)).get();

  REQUIRE(result == std::tuple{50, 70});
}

TEST_CASE("threads: a stop from another thread unwinds the chain on a worker, its frames with it",
          "[asio][threads][stopped]") {
  ::asio::thread_pool pool{4};
  // After the pool: its threads are joined first, as one may still be in the pool's post().
  stopping_threads threads;
  std::atomic<int> unwound{0};
  constexpr int tasks = 200;
  std::vector<std::future<bool>> ended;

  for (int index = 0; index < tasks; ++index) {
    auto promise = std::make_shared<std::promise<bool>>();

    ended.push_back(promise->get_future());
    co_spawn(quoll::asio::executor{pool.get_executor()}, stops_from_another_thread(unwound, threads),
             overloaded{[promise](int) { promise->set_value(false); },
                        [promise](std::exception_ptr error) { promise->set_exception(std::move(error)); },
                        [promise](quoll::async::stopped_t) { promise->set_value(true); }});
  }

  for (auto &one : ended) {
    REQUIRE(one.get());
  }

  // A worker may still be in its registration of a thread, the chain it stops unwound already on another one: the
  // workers are joined before the registry goes.
  pool.join();

  REQUIRE(unwound.load() == tasks);
}

TEST_CASE("slots: on many threads, one at a time holds a single place", "[asio][threads]") {
  ::asio::thread_pool pool{4};
  quoll::async::detail::slots places{1};
  // Not atomic: TSan reports it if two ever hold the place together.
  int count = 0;

  auto const take_turns = [](quoll::async::detail::slots &slots, int &counted) -> task<int> {
    for (int round = 0; round < 200; ++round) {
      co_await slots.acquire();
      ++counted;
      co_await quoll::async::yield();
      ++counted;
      slots.release();
    }

    co_return 0;
  };

  std::vector<std::future<int>> running;

  for (int worker = 0; worker < 4; ++worker) {
    running.push_back(spawn(pool, take_turns(places, count)));
  }

  for (auto &future : running) {
    REQUIRE(future.get() == 0);
  }

  REQUIRE(count == 4 * 200 * 2);
  REQUIRE(places.available() == 1);
}

TEST_CASE("slots: a stop from another thread ends a wait, as it starts or later", "[asio][threads]") {
  ::asio::thread_pool pool{4};

  for (int round = 0; round < 200; ++round) {
    quoll::async::detail::slots places{0};
    std::stop_source stopping;

    auto const waits = [](quoll::async::detail::slots &slots) -> task<int> {
      co_await slots.acquire();
      co_return 1;
    };

    // The wait ends on its stop, which its own chain absorbs.
    auto const stopped = [&waits](quoll::async::detail::slots &slots, std::stop_token token) -> task<int> {
      co_return (co_await quoll::async::detail::owned_chain<int>{waits(slots), std::move(token)}).value_or(2);
    };

    auto future = spawn(pool, stopped(places, stopping.get_token()));

    stopping.request_stop();
    REQUIRE(future.get() == 2);
  }
}
