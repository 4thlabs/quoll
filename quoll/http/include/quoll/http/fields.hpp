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

#ifndef QUOLL_HTTP_FIELDS_HPP
#define QUOLL_HTTP_FIELDS_HPP

#include <quoll/async/lifetimebound.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quoll::http {

/** @brief One header (or trailer) field. */
struct field {
  std::string name;
  std::string value;
};

/**
 * @brief Header or trailer fields of a message, in their order on the wire.
 *
 * Names compare case-insensitively. A name may appear several times: add() appends, set() replaces every field of
 * that name. Values are stored as received; the serializer rejects line breaks.
 *
 * Thread safety: none.
 */
class fields {
public:
  using value_type = field;
  using size_type = std::size_t;
  using const_iterator = std::vector<field>::const_iterator;
  using iterator = const_iterator;

  /** @brief Appends a field, keeping the ones of the same name. */
  void add(std::string_view name, std::string_view value) {
    fields_.push_back(field{std::string{name}, std::string{value}});
  }

  /** @brief Appends a field, keeping the ones of the same name. */
  void add(field entry) { fields_.push_back(std::move(entry)); }

  /** @brief Replaces every field named `name` by a single one. */
  void set(std::string_view name, std::string_view value);

  /** @brief Removes every field named `name`. @return The number removed. */
  size_type erase(std::string_view name) noexcept;

  /** @brief The value of the first field named `name`. */
  [[nodiscard]] std::optional<std::string_view> find(std::string_view name) const noexcept QUOLL_LIFETIMEBOUND;

  [[nodiscard]] bool contains(std::string_view name) const noexcept { return find(name).has_value(); }

  void clear() noexcept { fields_.clear(); }

  [[nodiscard]] const_iterator begin() const noexcept QUOLL_LIFETIMEBOUND { return fields_.begin(); }

  [[nodiscard]] const_iterator end() const noexcept QUOLL_LIFETIMEBOUND { return fields_.end(); }

  [[nodiscard]] size_type size() const noexcept { return fields_.size(); }

  [[nodiscard]] bool empty() const noexcept { return fields_.empty(); }

private:
  std::vector<field> fields_;
};

} // namespace quoll::http

#endif // QUOLL_HTTP_FIELDS_HPP
