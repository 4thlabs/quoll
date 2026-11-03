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

#ifndef QUOLL_CLIENT_DETAIL_CONNECTION_POOL_HPP
#define QUOLL_CLIENT_DETAIL_CONNECTION_POOL_HPP

#include <quoll/io/any_stream.hpp>

#include <cstddef>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace quoll::client::detail {

// The idle connections of a transport, between two requests. Thread safety: take() and give_back() may be called
// from any thread, concurrently. The lock serves the requests of one transport sent from several threads (a thread
// pool); with a single thread, as under WASM, it is never contended, and std::mutex needs no thread support there.
class connection_pool {
public:
  explicit connection_pool(std::size_t capacity) : capacity_(capacity) {}

  // The connection used last, the least likely to have been closed by the server for being idle.
  [[nodiscard]] std::optional<io::any_stream> take() {
    std::lock_guard const lock{mutex_};

    if (idle_.empty()) {
      return std::nullopt;
    }

    auto connection = std::move(idle_.back());

    idle_.pop_back();
    return connection;
  }

  // Keeps `connection` for a next request, or closes it if the pool is full.
  void give_back(io::any_stream connection) {
    std::lock_guard const lock{mutex_};

    if (idle_.size() < capacity_) {
      idle_.push_back(std::move(connection));
    }
  }

  // Closes the idle connections.
  void clear() {
    std::vector<io::any_stream> closed;

    {
      std::lock_guard const lock{mutex_};

      closed.swap(idle_);
    }
  }

private:
  std::mutex mutex_;
  std::vector<io::any_stream> idle_;
  std::size_t capacity_;
};

} // namespace quoll::client::detail

#endif // QUOLL_CLIENT_DETAIL_CONNECTION_POOL_HPP
