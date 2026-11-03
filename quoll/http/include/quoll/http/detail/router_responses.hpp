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

#ifndef QUOLL_HTTP_DETAIL_ROUTER_RESPONSES_HPP
#define QUOLL_HTTP_DETAIL_ROUTER_RESPONSES_HPP

#include <quoll/async/task.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>

#include <string>
#include <vector>

namespace quoll::http::detail {

// A task that produces `message` as soon as it is awaited: the router's own answers.
inline async::task<response> ready(response message) { co_return message; }

[[nodiscard]] inline response status_response(status code) {
  response message;

  message.status = code;
  return message;
}

// The value of `Allow`: "GET, DELETE".
[[nodiscard]] inline std::string join_methods(std::vector<verb> const &methods) {
  std::string joined;

  for (auto const method : methods) {
    joined += joined.empty() ? "" : ", ";
    joined += to_string(method);
  }

  return joined;
}

} // namespace quoll::http::detail

#endif // QUOLL_HTTP_DETAIL_ROUTER_RESPONSES_HPP
