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

#ifndef QUOLL_IO_DETAIL_MEMORY_CHANNEL_HPP
#define QUOLL_IO_DETAIL_MEMORY_CHANNEL_HPP

#include <quoll/async/executor_ref.hpp>
#include <quoll/async/work_item.hpp>
#include <quoll/io/error.hpp>
#include <quoll/io/io_result.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

namespace quoll::io::detail {

// One direction of a memory_stream pair: the bytes written by one end, waiting for the other end to read them, and
// the reader suspended until some arrive.
//
// A reader is woken through an item of the channel's own, never through the one in the reader's frame: a queue cannot
// take an item back, and the frame may be destroyed before its turn comes. While queued, that item keeps the channel
// alive, and when it runs it resumes the reader only if one that was woken is still there.
//
// The executor the wake was queued on has to run it: a read that comes after one gone while woken is resumed through
// that wake, queued again on its own executor. An executor that is no longer run, or drops a queued item without
// running it (an asio io_context destroyed with handlers pending), leaves later reads of that end waiting and the
// channel holding itself, so the pipe leaks: as with the coroutine frames such an executor leaves behind, run it until
// it is empty first.
class memory_channel {
public:
  memory_channel() noexcept {
    wake_.run = &run_wake;
    wake_.channel = this;
  }

  // The item it queues points back to it.
  memory_channel(memory_channel const &) = delete;
  memory_channel &operator=(memory_channel const &) = delete;
  memory_channel(memory_channel &&) = delete;
  memory_channel &operator=(memory_channel &&) = delete;
  ~memory_channel() = default;

  // A read is suspended, or woken and not resumed yet.
  [[nodiscard]] bool has_reader() const noexcept { return owner_ != nullptr; }

  // What it keeps, read or not: for a test.
  [[nodiscard]] std::size_t held() const noexcept { return bytes_.size(); }

  // Something to read now: bytes, or the end of the stream.
  [[nodiscard]] bool readable() const noexcept { return offset_ < bytes_.size() || closed_; }

  // Precondition: readable() or `buffer` is empty.
  [[nodiscard]] io_result read(std::span<std::byte> buffer) noexcept {
    if (buffer.empty()) {
      return {};
    }

    auto const available = bytes_.size() - offset_;

    if (available == 0) {
      assert(closed_ && "memory_channel::read() with nothing to read");
      return {error::end_of_stream, 0};
    }

    auto const count = std::min(buffer.size(), available);

    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_), count, buffer.begin());
    offset_ += count;

    // Past half, too: a reader always a little behind would otherwise keep all that ever went through.
    if (offset_ == bytes_.size()) {
      bytes_.clear();
      offset_ = 0;
    } else if (offset_ > bytes_.size() / 2) {
      bytes_.erase(bytes_.begin(), bytes_.begin() + static_cast<std::ptrdiff_t>(offset_));
      offset_ = 0;
    }

    return {{}, count};
  }

  [[nodiscard]] io_result write(std::span<std::byte const> data) {
    if (closed_ || !reader_alive_) {
      return {std::make_error_code(std::errc::broken_pipe), 0};
    }

    if (data.empty()) {
      return {};
    }

    bytes_.insert(bytes_.end(), data.begin(), data.end());

    // A write that throws must not have sent anything: the caller may retry it.
    try {
      wake();
    } catch (...) {
      bytes_.resize(bytes_.size() - data.size());
      throw;
    }

    return {{}, data.size()};
  }

  // No more bytes will be written: the reader gets the end of the stream once it has read the rest.
  void close() {
    closed_ = true;
    wake();
  }

  // The reading end is gone: writing fails from now on. A read still pending breaks the rules, which memory_stream
  // asserts; it is left as it is.
  void detach_reader() noexcept { reader_alive_ = false; }

  // `owner` owns this channel: a copy of it keeps the channel alive while its wake is queued.
  void wait(async::work_item &reader, async::executor_ref executor, std::shared_ptr<memory_channel> owner) noexcept {
    assert(owner_ == nullptr && "memory_stream: a read is already pending");
    reader_ = &reader;
    executor_.emplace(executor);
    owner_ = std::move(owner);
    woken_ = false;
  }

  // The read that waited has resumed, or is gone, woken or not: a wake still queued then finds no one.
  void end_wait() noexcept {
    reader_ = nullptr;
    owner_.reset();
    woken_ = false;
    queued_for_reader_ = false;
  }

  // Resumes the suspended read without bytes. False when there is none, or when it was already woken.
  [[nodiscard]] bool cancel() {
    if (reader_ == nullptr || woken_) {
      return false;
    }

    wake();
    return true;
  }

private:
  struct wake_item : async::work_item {
    memory_channel *channel = nullptr;
  };

  // The reader is marked woken only once its wake is queued: if post throws, it can still be woken later. The item is
  // in one queue at a time: queued for a reader that went, it queues itself again for the next one, if woken by the
  // time it runs, on that reader's executor.
  void wake() {
    if (reader_ == nullptr || woken_) {
      return;
    }

    if (!queued_) {
      queue();
    }

    woken_ = true;
  }

  // Only while a reader is there, whose owner and executor are alive.
  void queue() {
    queued_keep_ = owner_;

    try {
      executor_->post(wake_);
    } catch (...) {
      queued_keep_.reset();
      throw;
    }

    queued_ = true;
    queued_for_reader_ = true;
  }

  // What the resumed reader lets escape goes on to the executor's caller, as from any coroutine.
  static void run_wake(async::work_item &item) {
    auto &self = *static_cast<wake_item &>(item).channel;

    // The channel lives until this returns, though both ends and the reader may have gone.
    auto const keep = std::move(self.queued_keep_);

    self.queued_ = false;

    if (self.reader_ == nullptr || !self.woken_) {
      return;
    }

    // A reader that came after the one this was queued for resumes on its own executor. That post cannot report a
    // failure from here, and ends the program then, as an Executor says.
    if (!self.queued_for_reader_) {
      self.queue();
      return;
    }

    async::execute(*std::exchange(self.reader_, nullptr));
  }

  std::vector<std::byte> bytes_;
  std::size_t offset_ = 0;
  bool closed_ = false;
  bool reader_alive_ = true;
  bool woken_ = false;
  bool queued_ = false;
  bool queued_for_reader_ = false;
  async::work_item *reader_ = nullptr;
  std::optional<async::executor_ref> executor_;
  std::shared_ptr<memory_channel> owner_;
  wake_item wake_;
  std::shared_ptr<memory_channel> queued_keep_;
};

} // namespace quoll::io::detail

#endif // QUOLL_IO_DETAIL_MEMORY_CHANNEL_HPP
