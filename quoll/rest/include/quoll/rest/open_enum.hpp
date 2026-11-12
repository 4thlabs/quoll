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

#ifndef QUOLL_REST_OPEN_ENUM_HPP
#define QUOLL_REST_OPEN_ENUM_HPP

#include <quoll/async/lifetimebound.hpp>
#include <quoll/rest/param_traits.hpp>

#include <glaze/core/common.hpp>
#include <glaze/core/context.hpp>
#include <glaze/core/read.hpp>
#include <glaze/core/reflect.hpp>
#include <glaze/core/write.hpp>
#include <glaze/json/read.hpp>
#include <glaze/json/write.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace quoll::rest {

/**
 * @brief A value of `Enum` as a third-party API sends it, which may be a name the enum does not know: an API adds
 * values over time, and a response holding a new one still decodes.
 *
 * `Enum` is named by its `glz::meta`, as any enum Quoll reads, declared before the first open_enum of it. A name of
 * one of its enumerators reads as that one;
 * another is kept as it came, and written back as it came, so that what a program reads and sends again is unchanged.
 * Only the field of this type is open: the enum itself stays as strict as before, in a map key, another format, or a
 * contract of Quoll's own server. As JSON and as a query or header parameter.
 *
 * An enumerator with no name in the `glz::meta` (a cast, or `Enum{}` when no enumerator is 0) is no error until it is
 * written, which fails as it does for the enum.
 *
 * Thread safety: as a `std::string`, const functions may run on several threads at once.
 *
 * @code
 * struct issue {
 *   std::string title;
 *   quoll::rest::open_enum<issue_state> state;   // "open", "closed", or "merged" kept as it came
 * };
 *
 * if (found.state == issue_state::open) { ... }
 * std::cout << found.state.name() << '\n';
 * @endcode
 */
template <class Enum>
  requires std::is_enum_v<Enum> && glz::glaze_enum_t<Enum>
class open_enum {
public:
  /** @brief `Enum{}`, value-initialized, which has no name when no enumerator is 0. */
  open_enum() = default;

  /** @brief The enumerator `value`. */
  explicit open_enum(Enum value) noexcept : value_(value) {}

  /** @brief The enumerator named `name`, or `name` kept as a value the enum does not know. */
  [[nodiscard]] static open_enum named(std::string_view name) {
    if (auto const found = lookup(name)) {
      return open_enum{*found};
    }

    return kept(std::string{name});
  }

  /** @brief The enumerator, if the name is one of the enum's: none for a value the enum has no name for. */
  [[nodiscard]] std::optional<Enum> known() const noexcept {
    if (auto const *enumerator = std::get_if<Enum>(&value_);
        enumerator != nullptr && !glz::get_enum_name(*enumerator).empty()) {
      return *enumerator;
    }

    return std::nullopt;
  }

  /** @brief The name, as the API sent it or as the enum's `glz::meta` gives it: empty for a value it has no name for.
   */
  [[nodiscard]] std::string_view name() const noexcept QUOLL_LIFETIMEBOUND {
    if (auto const *enumerator = std::get_if<Enum>(&value_)) {
      return glz::get_enum_name(*enumerator);
    }

    return std::get<std::string>(value_);
  }

  /** @brief Whether both hold the same enumerator, or the same name kept. */
  [[nodiscard]] bool operator==(open_enum const &) const = default;

  /** @brief Whether it holds `enumerator`. */
  [[nodiscard]] bool operator==(Enum enumerator) const noexcept {
    auto const *held = std::get_if<Enum>(&value_);

    return held != nullptr && *held == enumerator;
  }

private:
  friend struct param_traits<open_enum>;
  friend struct glz::from<glz::JSON, open_enum>;
  friend struct glz::to<glz::JSON, open_enum>;

  // The enumerator whose name in the glz::meta is `name`, with no allocation: the names Glaze reads and writes.
  [[nodiscard]] static std::optional<Enum> lookup(std::string_view name) noexcept {
    std::optional<Enum> found;

    [&]<std::size_t... Index>(std::index_sequence<Index...>) {
      static_cast<void>(
          ((glz::reflect<Enum>::keys[Index] == name && (found = glz::get<Index>(glz::reflect<Enum>::values), true)) ||
           ...)
      );
    }(std::make_index_sequence<glz::reflect<Enum>::size>{});

    return found;
  }

  // As named, from a string the caller gives away: kept with no copy when the enum does not know it.
  [[nodiscard]] static open_enum from_name(std::string &&name) {
    if (auto const found = lookup(name)) {
      return open_enum{*found};
    }

    return kept(std::move(name));
  }

  [[nodiscard]] static open_enum kept(std::string &&name) noexcept {
    open_enum value;

    value.value_ = std::move(name);
    return value;
  }

  std::variant<Enum, std::string> value_{};
};

/** @brief An open_enum as a parameter: by its name, whatever it is, as long as it is text a `std::string` takes. */
template <class Enum>
struct param_traits<open_enum<Enum>> {
  /** @throws std::invalid_argument for an enumerator without a name. */
  [[nodiscard]] static std::string to_text(open_enum<Enum> const &value) {
    if (auto const *enumerator = std::get_if<Enum>(&value.value_)) {
      return param_traits<Enum>::to_text(*enumerator);
    }

    return std::get<std::string>(value.value_);
  }

  [[nodiscard]] static std::optional<open_enum<Enum>> from_text(std::string_view text) {
    auto checked = param_traits<std::string>::from_text(text);

    if (!checked) {
      return std::nullopt;
    }

    return open_enum<Enum>::from_name(std::move(*checked));
  }
};

} // namespace quoll::rest

// Glaze's reader and writer of an open_enum in JSON: a string, its name.
template <class Enum>
struct glz::from<glz::JSON, quoll::rest::open_enum<Enum>> {
  template <auto Opts>
  static void op(auto &value, glz::is_context auto &&ctx, auto &&it, auto end) {
    std::string name;

    glz::parse<glz::JSON>::op<Opts>(name, ctx, it, end);

    if (bool(ctx.error)) {
      return;
    }

    value = quoll::rest::open_enum<Enum>::from_name(std::move(name));
  }
};

// An enumerator goes through Glaze's writer of the enum, which refuses one without a name.
template <class Enum>
struct glz::to<glz::JSON, quoll::rest::open_enum<Enum>> {
  static constexpr bool can_error = true;

  template <auto Opts>
  static void op(auto &&value, auto &&...args) {
    std::visit([&](auto const &held) { glz::serialize<glz::JSON>::op<Opts>(held, args...); }, value.value_);
  }
};

#endif // QUOLL_REST_OPEN_ENUM_HPP
