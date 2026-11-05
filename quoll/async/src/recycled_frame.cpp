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

#include <quoll/async/detail/recycled_frame.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <new>
#include <utility>

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || defined(__SANITIZE_HWADDRESS__)
#define QUOLL_ASYNC_NO_FRAME_RECYCLING
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer) ||          \
    __has_feature(hwaddress_sanitizer)
#define QUOLL_ASYNC_NO_FRAME_RECYCLING
#endif
#endif

namespace quoll::async::detail {

#if defined(QUOLL_ASYNC_NO_FRAME_RECYCLING)

void *allocate_frame(std::size_t size) { return ::operator new(size); }

void deallocate_frame(void *frame, std::size_t) noexcept { ::operator delete(frame); }

bool frames_are_recycled() noexcept { return false; }

std::size_t frames_kept(std::size_t) noexcept { return 0; }

#else

namespace {

// In powers of two, from 64 bytes up to what a frame holding a read buffer takes. Past it, a frame is allocated as any
// other.
constexpr std::size_t smallest_size = 64;
constexpr std::size_t size_count = 8;
constexpr std::size_t largest_size = smallest_size << (size_count - 1);

// What a thread keeps of each size: a burst of connections goes back to the system, at most about 1 MiB stays.
constexpr std::size_t most_kept = 64;

// What a frame of this size takes, and which of the sizes that is.
[[nodiscard]] constexpr std::size_t rounded_size(std::size_t size) noexcept {
  return std::bit_ceil(std::max(size, smallest_size));
}

[[nodiscard]] constexpr std::size_t size_index(std::size_t size) noexcept {
  return static_cast<std::size_t>(std::countr_zero(rounded_size(size) / smallest_size));
}

static_assert(size_index(1) == 0 && size_index(64) == 0 && size_index(65) == 1 && size_index(128) == 1);
static_assert(size_index(129) == 2 && size_index(largest_size) == size_count - 1);

struct free_frame {
  free_frame *next;
};

// Set once this thread's frames are gone: a frame freed by what ends after them goes back to the system.
constinit thread_local bool frames_gone = false;

class thread_frames {
public:
  thread_frames() noexcept = default;
  thread_frames(thread_frames const &) = delete;
  thread_frames &operator=(thread_frames const &) = delete;
  thread_frames(thread_frames &&) = delete;
  thread_frames &operator=(thread_frames &&) = delete;

  ~thread_frames() {
    frames_gone = true;

    for (auto *frame : free_) {
      while (frame != nullptr) {
        ::operator delete(std::exchange(frame, frame->next));
      }
    }
  }

  [[nodiscard]] void *take(std::size_t index) noexcept {
    auto *const frame = free_[index];

    if (frame != nullptr) {
      free_[index] = frame->next;
      --kept_[index];
    }

    return frame;
  }

  [[nodiscard]] bool keep(void *memory, std::size_t index) noexcept {
    if (kept_[index] == most_kept) {
      return false;
    }

    free_[index] = ::new (memory) free_frame{free_[index]};
    ++kept_[index];
    return true;
  }

  [[nodiscard]] std::size_t kept(std::size_t index) const noexcept { return kept_[index]; }

private:
  std::array<free_frame *, size_count> free_{};
  std::array<std::size_t, size_count> kept_{};
};

constinit thread_local thread_frames frames;

} // namespace

void *allocate_frame(std::size_t size) {
  if (size > largest_size) {
    return ::operator new(size);
  }

  auto const index = size_index(size);

  if (!frames_gone) {
    if (auto *const frame = frames.take(index)) {
      return frame;
    }
  }

  // Its whole size, for whichever frame of that size takes it next.
  return ::operator new(rounded_size(size));
}

void deallocate_frame(void *frame, std::size_t size) noexcept {
  if (size > largest_size || frames_gone || !frames.keep(frame, size_index(size))) {
    ::operator delete(frame);
  }
}

bool frames_are_recycled() noexcept { return true; }

std::size_t frames_kept(std::size_t size) noexcept {
  return size > largest_size || frames_gone ? 0 : frames.kept(size_index(size));
}

#endif

} // namespace quoll::async::detail
