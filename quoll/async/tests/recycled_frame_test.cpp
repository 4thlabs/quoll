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

#include <quoll/async/detail/detached.hpp>
#include <quoll/async/detail/recycled_frame.hpp>
#include <quoll/async/run_loop.hpp>
#include <quoll/async/task.hpp>
#include <quoll/test/run.hpp>

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <cstddef>
#include <set>
#include <thread>
#include <vector>

namespace {

using quoll::async::task;
using quoll::async::detail::allocate_frame;
using quoll::async::detail::deallocate_frame;
using quoll::async::detail::frames_are_recycled;
using quoll::async::detail::recycled_frame;

task<int> add(int left, int right) { co_return left + right; }

task<int> nested(int depth) {
  if (depth == 0) {
    co_return 0;
  }

  co_return 1 + co_await nested(depth - 1);
}

} // namespace

TEST_CASE("recycled frames: a freed frame serves the next of its size", "[async]") {
  auto *const first = allocate_frame(100);

  deallocate_frame(first, 100);

  auto *const second = allocate_frame(120);

  // Both round up to one size, which is what a recycled frame relies on to fit.
  if (frames_are_recycled()) {
    REQUIRE(first == second);
  }

  deallocate_frame(second, 120);
}

TEST_CASE("recycled frames: every size is served from what holds it", "[async]") {
  // A frame freed is taken again by what its size rounds up to, and by nothing larger: larger, it would not fit.
  struct expected {
    std::size_t freed;
    std::size_t asked;
    bool same;
  };

  for (auto const [freed, asked, same] :
       {expected{64, 1, true}, expected{64, 65, false}, expected{128, 65, true}, expected{128, 129, false},
        expected{256, 129, true}, expected{4096, 4096, true}, expected{4096, 4097, false}, expected{8192, 4097, true},
        expected{8192, 8193, false}}) {
    CAPTURE(freed, asked);

    auto *const first = allocate_frame(freed);

    deallocate_frame(first, freed);

    auto *const second = static_cast<char *>(allocate_frame(asked));

    // All of what it asked for is its own to write.
    second[asked - 1] = 'z';

    if (frames_are_recycled()) {
      REQUIRE((first == second) == same);
    }

    deallocate_frame(second, asked);
  }
}

TEST_CASE("recycled frames: past what a thread keeps, frames go back to the system", "[async]") {
  std::vector<void *> many;

  for (auto index = 0; index < 1000; ++index) {
    many.push_back(allocate_frame(256));
  }

  std::set<void *> const freed(many.begin(), many.end());

  for (auto *const frame : many) {
    deallocate_frame(frame, 256);
  }

  many.clear();

  // At most what it kept comes from there: the system may give some of the others back too, but not one in a row from a
  // list of a thousand, as would come of a thread that kept them all.
  auto const kept = allocate_frame(256);

  many.push_back(kept);

  for (auto index = 1; index < 1000; ++index) {
    many.push_back(allocate_frame(256));
  }

  if (frames_are_recycled()) {
    REQUIRE(freed.contains(kept));

    // Those it kept are the last it freed, in reverse: what follows them is another story.
    auto in_a_row = std::size_t{1};

    while (in_a_row < many.size() && freed.contains(many[in_a_row]) && many[in_a_row] < many[in_a_row - 1]) {
      ++in_a_row;
    }

    REQUIRE(in_a_row <= 64);
  }

  for (auto *const frame : many) {
    deallocate_frame(frame, 256);
  }
}

// The frames of a promise are recycled: what bench_allocations would only show at night.
static_assert(std::derived_from<task<>::promise_type, recycled_frame>);
static_assert(std::derived_from<task<int>::promise_type, recycled_frame>);
static_assert(std::derived_from<quoll::async::detail::detached::promise_type, recycled_frame>);

TEST_CASE("recycled frames: coroutines run as ever, nested and in turn", "[async]") {
  for (auto index = 0; index < 100; ++index) {
    REQUIRE(quoll::test::run(add(index, 1)) == index + 1);
  }

  REQUIRE(quoll::test::run(nested(200)) == 200);
}

// A thread: none under WASM here.
#if !defined(__EMSCRIPTEN__)

namespace {

// Built before the frames of its thread are touched, and so destroyed after them.
class after_the_frames {
public:
  explicit after_the_frames(bool &done) noexcept : done_(&done) {}

  after_the_frames(after_the_frames const &) = delete;
  after_the_frames &operator=(after_the_frames const &) = delete;
  after_the_frames(after_the_frames &&) = delete;
  after_the_frames &operator=(after_the_frames &&) = delete;

  ~after_the_frames() {
    auto *const frame = allocate_frame(200);

    deallocate_frame(frame, 200);
    *done_ = true;
  }

private:
  bool *done_;
};

} // namespace

TEST_CASE("recycled frames: a thread ending frees and takes frames past its own", "[async]") {
  auto done = false;

  std::thread ending{[&done] {
    thread_local after_the_frames const later{done};

    // Now the frames of this thread come after `later`, and go before it.
    deallocate_frame(allocate_frame(200), 200);
  }};

  ending.join();
  REQUIRE(done);
}

TEST_CASE("recycled frames: a frame freed on another thread is that thread's to use", "[async]") {
  auto *const frame = allocate_frame(200);
  void *reused = nullptr;

  std::thread other{[&] {
    deallocate_frame(frame, 200);
    reused = allocate_frame(200);
    deallocate_frame(reused, 200);
  }};

  other.join();

  if (frames_are_recycled()) {
    REQUIRE(reused == frame);
  }
}

#endif
