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

#include <quoll/rest/detail/arguments.hpp>

#include <quoll/http/detail/ascii.hpp>

#include <algorithm>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace quoll::rest::detail {

std::expected<std::tuple<>, argument_error> decode_all(type_list<>, request_input const &) { return std::tuple<>{}; }

std::vector<std::string_view> header_texts(http::fields const &fields, std::string_view name) {
  std::vector<std::string_view> texts;

  for (auto const &entry : fields) {
    if (http::detail::iequals(entry.name, name)) {
      texts.push_back(entry.value);
    }
  }

  return texts;
}

std::string_view media_type(std::string_view content_type) noexcept {
  return http::detail::trim(content_type.substr(0, content_type.find(';')));
}

std::vector<std::string_view> list_elements(std::vector<std::string_view> const &lines) {
  std::vector<std::string_view> elements;

  for (auto const line : lines) {
    for (auto const element : std::views::split(line, ',')) {
      auto const text = http::detail::trim(std::string_view{element.begin(), element.end()});

      if (!text.empty()) {
        elements.push_back(text);
      }
    }
  }

  return elements;
}

} // namespace quoll::rest::detail
