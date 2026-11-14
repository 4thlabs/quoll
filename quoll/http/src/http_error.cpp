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

#include <quoll/http/http_error.hpp>

#include <quoll/http/detail/error_answer.hpp>
#include <quoll/http/ensure_ok.hpp>
#include <quoll/http/problem.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace quoll::http {

http_error http_error::received_from(http::response answer, std::string const &detail) {
  http_error error{answer.status, detail};

  error.response_ = std::make_shared<http::response const>(std::move(answer));
  return error;
}

response ensure_ok(response answer) {
  if (static_cast<int>(answer.status) < 400) {
    return answer;
  }

  auto const code = static_cast<int>(answer.status);
  auto const reason = reason_phrase(answer.status);
  auto detail = std::to_string(code);

  if (!reason.empty()) {
    detail += ' ';
    detail += reason;
  }

  throw http_error::received_from(std::move(answer), detail);
}

std::optional<response> detail::error_answer(http_error const &error) {
  if (error.received()) {
    return problem_response(status::bad_gateway, "a call to another server failed");
  }

  auto const code = static_cast<int>(error.status());

  if (code < 400 || code > 599) {
    return std::nullopt;
  }

  return problem_response(error.status(), error.what());
}

} // namespace quoll::http
