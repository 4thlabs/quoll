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

#include <quoll/http/fields.hpp>

#include <quoll/http/detail/ascii.hpp>

#include <algorithm>
#include <functional>
#include <string>
#include <utility>

namespace quoll::http {

void fields::set(std::string_view name, std::string_view value) {
  // Either may view a field of this list, which erase() moves or destroys.
  field entry{std::string{name}, std::string{value}};

  erase(entry.name);
  add(std::move(entry));
}

namespace {

[[nodiscard]] bool views_into(std::string_view view, std::string const &text) noexcept {
  std::less<char const *> const before;

  return !before(view.data(), text.data()) && before(view.data(), text.data() + text.size() + 1);
}

} // namespace

fields::size_type fields::erase(std::string_view name) {
  auto const remove = [this](std::string_view what) {
    return std::erase_if(fields_, [what](field const &entry) { return detail::iequals(entry.name, what); });
  };

  // What it views may be a field of this list, which removing moves over.
  auto const aliased = std::ranges::any_of(
      fields_, [name](field const &entry) { return views_into(name, entry.name) || views_into(name, entry.value); });

  return aliased ? remove(std::string{name}) : remove(name);
}

std::optional<std::string_view> fields::find(std::string_view name) const noexcept {
  auto const found =
      std::ranges::find_if(fields_, [name](field const &entry) { return detail::iequals(entry.name, name); });

  if (found == fields_.end()) {
    return std::nullopt;
  }

  return std::string_view{found->value};
}

} // namespace quoll::http
