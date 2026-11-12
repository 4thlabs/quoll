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

#ifndef QUOLL_TEST_RUN_TASK_HPP
#define QUOLL_TEST_RUN_TASK_HPP

#include <quoll/asio/runtime.hpp>
#include <quoll/async/co_spawn.hpp>
#include <quoll/async/stopped.hpp>
#include <quoll/async/task.hpp>
#include <quoll/test/overloaded.hpp>

#include <asio/io_context.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/ip/tcp.hpp>

#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

namespace quoll::test {

/** @brief Runs `work` as a program would, on a runtime stopped by Ctrl-C, and hands over its outcome. */
template <class T>
T run_task(async::task<T> work) {
  asio::runtime runtime;
  std::optional<T> value;
  std::exception_ptr failure;
  auto stopped = false;

  co_spawn(
      runtime.get_executor(), std::move(work),
      overloaded{
          [&](T result) { value = std::move(result); }, [&](std::exception_ptr error) { failure = std::move(error); },
          [&](async::stopped_t) { stopped = true; }
      },
      runtime.get_stop_token()
  );
  runtime.run();

  if (failure) {
    std::rethrow_exception(failure);
  }

  if (stopped) {
    throw std::runtime_error{"quoll::test::run_task: the task ended on a stop"};
  }

  return std::move(value.value());
}

/**
 * @brief A port free on the loopback interface, as the system picks it: bound, read, released. Another process could
 * take it before the test uses it; on a test machine's loopback, that is unlikely enough.
 */
[[nodiscard]] inline std::uint16_t free_port() {
  ::asio::io_context context;
  ::asio::ip::tcp::acceptor const acceptor{context, ::asio::ip::tcp::endpoint{::asio::ip::address_v4::loopback(), 0}};

  return acceptor.local_endpoint().port();
}

} // namespace quoll::test

#endif // QUOLL_TEST_RUN_TASK_HPP
