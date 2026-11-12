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

#ifndef QUOLL_UI_DATA_FORM_STATE_HPP
#define QUOLL_UI_DATA_FORM_STATE_HPP

#include <quoll/ui/detail/data/state_link.hpp>
#include <quoll/ui/detail/for_each_member.hpp>
#include <quoll/ui/detail/text_value.hpp>
#include <quoll/ui/view/decorators.hpp>
#include <quoll/ui/view/element.hpp>

#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace quoll::ui {

namespace detail {

// The field of props a form_state edits: its type and the props it is in, or none for what is not a field.
template <class M>
struct form_field {
  static constexpr bool valid = false;
  using value_type = void;
  using props_type = void;
};

template <class T, class P>
  requires(!std::is_function_v<T>)
struct form_field<T P::*> {
  static constexpr bool valid = true;
  using value_type = T;
  using props_type = P;
};

} // namespace detail

/**
 * @brief What a form edits: a value its component's props give, a field of them, as the fields of the form show it,
 * with what is typed in them, as React Hook Form's defaultValues with keepDirtyValues.
 *
 * @code
 * struct product_detail {
 *   struct props {
 *     products::product item;
 *   };
 *
 *   shared<catalog> products;
 *   form_state<&props::item> form;
 *   action<&catalog::save> save{products};
 *
 *   element view(props const &given) {
 *     return section({
 *         label({text("Price"), input() | bind(form, &products::product::price)}),
 *         when(!form.valid(), [] { return p("The price is a number"); }),
 *         button("Save") | disabled(save->loading || !form.valid()) | on_click(save.with(form)),
 *     });
 *   }
 * };
 * @endcode
 *
 * A member of a component with props, `Member` the field of them it edits, a struct whose fields are given to inputs
 * with bind(): a field not typed in shows what the props hold, one typed in what is typed, kept as it is, even what
 * does not read as the field's type, as `12,5` for a number. value() is what the props give, with each field typed in
 * that reads; valid() whether every field typed in reads; `action::with(form)` runs an action with value() as it is
 * at the click, a click refused while it is not valid. The value comes from the props before each view of its
 * component, as the view shows it: when they give another, as once what was saved comes back, a field typed in whose
 * text is what it would show of it is no longer typed in, and the others stay as they are, as typed while the save was
 * on its way, or `2.50` for a `2.5`, which stays as typed.
 * Another value to edit from the start, as another product, is another component, placed with a key of its own.
 *
 * Neither copied nor moved: the inputs bound to it refer to it. Thread safety: none, as a state.
 */
template <auto Member>
class form_state {
  using traits = detail::form_field<decltype(Member)>;

  static_assert(traits::valid, "quoll::ui::form_state: of a field of its component's props, as &props::item");

public:
  using value_type = typename traits::value_type;
  using props_type = typename traits::props_type;

private:
  static_assert(std::is_aggregate_v<value_type> && std::copyable<value_type> && std::default_initializable<value_type>,
                "quoll::ui::form_state: what it edits is a struct, copied from the props");

  static constexpr std::size_t count = detail::member_count<value_type>;

public:
  form_state() = default;

  form_state(form_state const &) = delete;
  form_state(form_state &&) = delete;
  form_state &operator=(form_state const &) = delete;
  form_state &operator=(form_state &&) = delete;

  ~form_state() = default;

  /** @brief What the props give, with each field typed in that reads as its type. */
  [[nodiscard]] value_type value() const {
    auto edited = base_;

    detail::for_each_member_at(edited, [this](std::size_t index, auto &field) {
      using type = std::remove_cvref_t<decltype(field)>;

      if constexpr (detail::text_value<type>) {
        if (typed_[index]) {
          if (auto read = detail::read_text<type>(*typed_[index])) {
            field = std::move(*read);
          }
        }
      }
    });

    return edited;
  }

  /** @brief Whether each field typed in reads as its type, as a number for a number. */
  [[nodiscard]] bool valid() const {
    bool reads = true;

    detail::for_each_member_at(base_, [this, &reads](std::size_t index, auto const &field) {
      using type = std::remove_cvref_t<decltype(field)>;

      if constexpr (detail::text_value<type>) {
        if (typed_[index] && !detail::read_text<type>(*typed_[index])) {
          reads = false;
        }
      }
    });

    return reads;
  }

  /**
   * @brief For its component, before each of its views: the value of `given` is what it edits now, and a field typed
   * in whose text is the one it shows of that value is no longer typed in.
   */
  void before_view(props_type const &given) {
    auto const &now = given.*Member;

    if constexpr (std::equality_comparable<value_type>) {
      if (given_ && now == base_) {
        return;
      }
    }

    base_ = now;
    given_ = true;

    detail::for_each_member_at(base_, [this](std::size_t index, auto const &field) {
      using type = std::remove_cvref_t<decltype(field)>;

      if constexpr (detail::text_value<type>) {
        // By the text, not the value read: `2.50` typed is not `2.5` shown, nor `3.0` on its way to `3.05` a `3`.
        if (typed_[index] && *typed_[index] == detail::write_text(field)) {
          typed_[index].reset();
        }
      }
    });
  }

  /** @brief For the app of the component holding it, as a state's: what is typed shows it again. */
  void link(detail::state_link to) noexcept { link_ = to; }

  /**
   * @brief Keeps an input and the field `member` of what `form` edits the same: the input shows the field, or what is
   * typed in it, and typing changes it. A field of a string or a number goes to an input, whose text it reads; one of a
   * bool to a checkbox, of the type it gives an input given none, as bind(state<bool>).
   * @throws std::invalid_argument for no field, a null pointer.
   */
  template <class F>
  [[nodiscard]] friend auto bind(form_state &form, F value_type::*member) {
    static_assert(detail::text_value<F>, "quoll::ui::bind: a field of a form is a string, a number or a bool");

    auto const index = form.index_of(member);

    return decorator([&form, member, index](element &target) {
      if constexpr (std::same_as<F, bool>) {
        target.attributes.try_emplace("type", "checkbox");
        target.checked = form.typed_[index] ? *form.typed_[index] == "true" : form.base_.*member;
        target.events.push_back({.name = "change", .handler = form.typing(index)});
      } else {
        target.value = form.typed_[index] ? *form.typed_[index] : detail::write_text(form.base_.*member).value_or("");
        target.events.push_back({.name = "input", .handler = form.typing(index)});
      }
    });
  }

private:
  // Where `member` is among the fields of what it edits, which it finds by their address in it.
  template <class F>
  [[nodiscard]] std::size_t index_of(F value_type::*member) const {
    if (member == nullptr) {
      throw std::invalid_argument{"quoll::ui::bind: no field of the form"};
    }

    auto const *const address = static_cast<void const *>(std::addressof(base_.*member));
    std::size_t found = count;

    detail::for_each_member_at(base_, [address, &found](std::size_t index, auto const &field) {
      if (static_cast<void const *>(std::addressof(field)) == address) {
        found = index;
      }
    });

    // A pointer to a member of value_type, which bind() takes, is one of its fields.
    assert(found < count && "quoll::ui::form_state: a field of what it edits");
    return found;
  }

  // What typing in the field `index` runs: it holds what is typed, and its component shows again.
  [[nodiscard]] event_handler typing(std::size_t index) {
    return detail::to_handler([this, index](std::string typed) {
      typed_[index] = std::move(typed);
      detail::tell(link_);
    });
  }

  value_type base_{};
  // Whether its props were given once: before, base_ holds none of theirs.
  bool given_ = false;
  // What is typed in each field, none for one not typed in.
  std::array<std::optional<std::string>, count> typed_{};
  detail::state_link link_{};
};

} // namespace quoll::ui

#endif // QUOLL_UI_DATA_FORM_STATE_HPP
