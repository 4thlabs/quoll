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

#include <quoll/rest/detail/client_request.hpp>
#include <quoll/rest/detail/client_response.hpp>

#include <quoll/http/detail/ascii.hpp>
#include <quoll/http/detail/plain_segment.hpp>
#include <quoll/rest/json_codec.hpp>
#include <quoll/rest/problem.hpp>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace quoll::rest::detail {

void check_path_text(std::string_view name, std::string_view text, bool rest_of_path) {
  bool const in_place = rest_of_path ? !text.starts_with('/') : !text.empty();

  if (!in_place || !http::detail::plain_segment(text)) {
    throw std::invalid_argument{"quoll::rest::client: invalid path parameter " + std::string{name}};
  }
}

std::string failure_detail(unexpected_response const &failure) {
  auto const content_type = failure.response.fields.find("Content-Type");

  if (content_type && http::detail::iequals(media_type(*content_type), problem_content_type)) {
    if (auto const decoded = lenient_json_codec{}.decode<problem>(failure.response.body); decoded && decoded->detail) {
      return *decoded->detail;
    }
  }

  return failure.reason;
}

void fail(unexpected_response failure) {
  auto detail = failure_detail(failure);

  throw call_error{std::move(failure), detail};
}

} // namespace quoll::rest::detail
