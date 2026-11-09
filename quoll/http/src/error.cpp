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

#include <quoll/http/error.hpp>

#include <string>

namespace quoll::http {

namespace {

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
    case error::unsupported_expectation:
      return "unsupported HTTP expectation";
    case error::too_many_redirects:
      return "too many HTTP redirects";
    case error::event_too_large:
      return "server-sent event too large";
    case error::websocket_protocol:
      return "WebSocket protocol error";
    case error::websocket_invalid_text:
      return "WebSocket text that is not UTF-8";
    case error::websocket_too_large:
      return "WebSocket message too large";
    }
    return "unknown quoll.http error";
  }
};

} // namespace

std::error_category const &http_category() noexcept {
  static http_error_category const category;
  return category;
}

} // namespace quoll::http
