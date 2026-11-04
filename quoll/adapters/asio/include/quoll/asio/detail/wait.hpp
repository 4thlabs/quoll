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

#ifndef QUOLL_ASIO_DETAIL_WAIT_HPP
#define QUOLL_ASIO_DETAIL_WAIT_HPP

#include <quoll/asio/detail/cancellation.hpp>
#include <quoll/asio/detail/operation.hpp>
#include <quoll/async/task.hpp>

#include <asio/any_io_executor.hpp>
#include <asio/error.hpp>
#include <asio/steady_timer.hpp>

#include <chrono>
#include <memory>
#include <system_error>
#include <utility>

namespace quoll::asio::detail {

// True once `duration` has elapsed, false on a stop request (see async::Timer). asio saturates a duration too long for
// its clock.
[[nodiscard]] inline async::task<bool> wait(::asio::any_io_executor executor, std::chrono::nanoseconds duration) {
  ::asio::steady_timer timer{executor, std::chrono::duration_cast<::asio::steady_timer::duration>(duration)};

  auto const [error] = co_await make_operation<>(std::make_shared<cancellation>(std::move(executor)),
                                                 [&timer](auto handler) { timer.async_wait(std::move(handler)); });

  if (error == ::asio::error::operation_aborted) {
    co_return false;
  }

  if (error) {
    throw std::system_error{error, "quoll::asio::executor::wait_for"};
  }

  co_return true;
}

} // namespace quoll::asio::detail

#endif // QUOLL_ASIO_DETAIL_WAIT_HPP
