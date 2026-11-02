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

#ifndef QUOLL_HTTP_ERROR_HPP
#define QUOLL_HTTP_ERROR_HPP

#include <string>
#include <system_error>
#include <type_traits>

namespace quoll::http {

/** @brief Why a message could not be parsed. Parse failures are expected outcomes: error codes, not exceptions. */
enum class error {
  /** @brief Not valid HTTP/1.x; the parser's error_reason() says why. */
  bad_message = 1,
  /** @brief The start line and fields exceed parse_limits::max_header_size. */
  header_too_large,
  /** @brief The body exceeds parse_limits::max_body_size. */
  body_too_large,
  /** @brief A request method outside http::verb. */
  unsupported_method,
  /** @brief The connection ended in the middle of a message. */
  partial_message,
  /** @brief The connection switched protocols (see parser::upgrade()): what follows is not HTTP/1.x. */
  upgraded,
};

namespace detail {

// std::error_category is polymorphic by design: the virtual functions here are the standard's, not Quoll's.
class http_error_category final : public std::error_category {
public:
  [[nodiscard]] char const *name() const noexcept override { return "quoll.http"; }

  [[nodiscard]] std::string message(int value) const override {
    switch (static_cast<error>(value)) {
    case error::bad_message:
      return "malformed HTTP message";
    case error::header_too_large:
      return "HTTP header too large";
    case error::body_too_large:
      return "HTTP body too large";
    case error::unsupported_method:
      return "unsupported HTTP method";
    case error::partial_message:
      return "HTTP message truncated by the end of the stream";
    case error::upgraded:
      return "HTTP connection switched to another protocol";
    }
    return "unknown quoll.http error";
  }
};

} // namespace detail

/** @brief The category of http::error codes. */
[[nodiscard]] inline std::error_category const &http_category() noexcept {
  static detail::http_error_category const category;
  return category;
}

[[nodiscard]] inline std::error_code make_error_code(error value) noexcept {
  return {static_cast<int>(value), http_category()};
}

} // namespace quoll::http

template <>
struct std::is_error_code_enum<quoll::http::error> : std::true_type {};

#endif // QUOLL_HTTP_ERROR_HPP
