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
#include <quoll/io/error.hpp>
#include <quoll/io/io_result.hpp>

#include <algorithm>
#include <cassert>
#include <coroutine>
#include <cstddef>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

namespace quoll::io::detail {

// One direction of a memory_stream pair: the bytes written by one end, waiting for the other end to read them, and
// the reader suspended until some arrive.
class memory_channel {
public:
  // A read is suspended, or woken and not resumed yet.
  [[nodiscard]] bool has_reader() const noexcept { return reading_; }

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

    if (offset_ == bytes_.size()) {
      bytes_.clear();
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

  // The reading end is gone: writing fails from now on.
  void detach_reader() noexcept {
    reader_alive_ = false;
    reader_ = {};
    reading_ = false;
  }

  void wait(std::coroutine_handle<> reader, async::executor_ref executor) noexcept {
    assert(!reading_ && "memory_stream: a read is already pending");
    reader_ = reader;
    executor_.emplace(executor);
    reading_ = true;
  }

  // The read that waited has resumed.
  void end_wait() noexcept {
    reader_ = {};
    reading_ = false;
  }

  // Resumes the suspended read without bytes. False when there is none, or when it was already woken.
  [[nodiscard]] bool cancel() {
    if (!reader_) {
      return false;
    }

    wake();
    return true;
  }

private:
  // The handle is forgotten only once posted: if post throws, the reader can still be woken later.
  void wake() {
    if (reader_) {
      executor_->post(reader_);
      reader_ = {};
    }
  }

  std::vector<std::byte> bytes_;
  std::size_t offset_ = 0;
  bool closed_ = false;
  bool reader_alive_ = true;
  bool reading_ = false;
  std::coroutine_handle<> reader_;
  std::optional<async::executor_ref> executor_;
};

} // namespace quoll::io::detail

#endif // QUOLL_IO_DETAIL_MEMORY_CHANNEL_HPP
