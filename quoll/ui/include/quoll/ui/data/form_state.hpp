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
#include <quoll/ui/detail/text_value.hpp>
#include <quoll/ui/view/decorators.hpp>
#include <quoll/ui/view/element.hpp>

#include <algorithm>
#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace quoll::ui {

/**
 * @brief What a form edits: a value its component's props give, a field of them, as the fields of the form show it,
 * with what is typed in them, as React Hook Form's defaultValues with keepDirtyValues.
 *
 * @code
 * inline constexpr component<product_detail_props> product_detail = [](product_detail_props const &given) {
 *   auto &products = use_context<catalog>();
 *   auto &form = use_form(given.item);
 *   auto &save = use_action<&catalog::save>(products);
 *
 *   return section({
 *       label({text("Price"), input() | bind(form, &products::product::price)}),
 *       when(!form.valid(), [] { return p("The price is a number"); }),
 *       button("Save") | disabled(save->loading || !form.valid()) | on_click(save.with(form)),
 *   });
 * };
 * @endcode
 *
 * What use_form() gives a component, whose fields are given to inputs with
 * bind(): a field not typed in shows what its component gives, one typed in what is typed, kept as it is, even what
 * does not read as the field's type, as `12,5` for a number. value() is what it is given, with each field typed in
 * that reads; valid() whether every field typed in reads; `action::with(form)` runs an action with value() as it is
 * at the click, a click refused while it is not valid. The value comes before each view of its component, as the view
 * shows it: when it is given another, as once what was saved comes back, a field typed in whose
 * text is what it would show of it is no longer typed in, and the others stay as they are, as typed while the save was
 * on its way, or `2.50` for a `2.5`, which stays as typed.
 * Another value to edit from the start, as another product, is another component, placed with a key of its own.
 *
 * Neither copied nor moved: the inputs bound to it refer to it. Thread safety: none, as a state.
 */
template <class T>
class form_state {
  static_assert(
      std::copyable<T> && std::default_initializable<T>, "quoll::ui::form_state: what it edits is copied as it is given"
  );

public:
  using value_type = T;

  form_state() = default;

  form_state(form_state const &) = delete;
  form_state(form_state &&) = delete;
  form_state &operator=(form_state const &) = delete;
  form_state &operator=(form_state &&) = delete;

  ~form_state() = default;

  /** @brief What it is given, with each field typed in that reads as its type. */
  [[nodiscard]] value_type value() const {
    auto edited = base_;

    for (auto const &field : typed_) {
      field.write(edited, field.text);
    }

    return edited;
  }

  /** @brief Whether each field typed in reads as its type, as a number for a number. */
  [[nodiscard]] bool valid() const {
    return std::ranges::all_of(typed_, [](typed_field const &field) { return field.reads(field.text); });
  }

  /**
   * @brief What it edits now, `now`, as its component's view is about to run: a field typed in whose text is the one it
   * shows of that value is no longer typed in.
   */
  void take(value_type const &now) {
    if constexpr (std::equality_comparable<value_type>) {
      if (given_ && now == base_) {
        return;
      }
    }

    base_ = now;
    given_ = true;

    // By the text, not the value read: `2.50` typed is not `2.5` shown, nor `3.0` on its way to `3.05` a `3`.
    std::erase_if(typed_, [this](typed_field const &field) { return field.shown(base_, field.text); });
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

    if (member == nullptr) {
      throw std::invalid_argument{"quoll::ui::bind: no field of the form"};
    }

    return decorator([&form, member](element &target) {
      auto const *const typed = form.typed_text(member);

      if constexpr (std::same_as<F, bool>) {
        target.attributes.try_emplace("type", "checkbox");
        target.checked = typed != nullptr ? *typed == "true" : form.base_.*member;
        target.events.push_back({.name = "change", .handler = form.typing(member)});
      } else {
        target.value = typed != nullptr ? *typed : detail::write_text(form.base_.*member).value_or("");
        target.events.push_back({.name = "input", .handler = form.typing(member)});
      }
    });
  }

private:
  // A field typed in: where it is in base_, which stays where it is as it is given another value, what is typed, and
  // what reads that text, writes it into a value and tells whether a value shows it, for the field's own type.
  struct typed_field {
    void const *field;
    std::string text;
    bool (*reads)(std::string_view text);
    std::function<void(value_type &, std::string_view)> write;
    std::function<bool(value_type const &, std::string_view)> shown;
  };

  template <class F>
  [[nodiscard]] typed_field *find(F value_type::*member) noexcept {
    auto const *const address = static_cast<void const *>(std::addressof(base_.*member));
    auto const found = std::ranges::find(typed_, address, &typed_field::field);

    return found == typed_.end() ? nullptr : &*found;
  }

  template <class F>
  [[nodiscard]] std::string const *typed_text(F value_type::*member) noexcept {
    auto const *const found = find(member);

    return found == nullptr ? nullptr : &found->text;
  }

  // What typing in the field `member` runs: it holds what is typed, and its component shows again.
  template <class F>
  [[nodiscard]] event_handler typing(F value_type::*member) {
    return detail::to_handler([this, member](std::string typed) {
      if (auto *const found = find(member); found != nullptr) {
        found->text = std::move(typed);
      } else {
        typed_.push_back({
            .field = std::addressof(base_.*member),
            .text = std::move(typed),
            .reads = [](std::string_view text) { return detail::read_text<F>(text).has_value(); },
            .write =
                [member](value_type &into, std::string_view text) {
                  if (auto read = detail::read_text<F>(text)) {
                    into.*member = std::move(*read);
                  }
                },
            .shown = [member](
                         value_type const &from, std::string_view text
                     ) { return text == detail::write_text(from.*member); },
        });
      }

      detail::tell(link_);
    });
  }

  value_type base_{};
  // Whether it was given a value once: before, base_ holds none.
  bool given_ = false;
  // The fields typed in, in the order they were first typed in.
  std::vector<typed_field> typed_;
  detail::state_link link_{};
};

} // namespace quoll::ui

#endif // QUOLL_UI_DATA_FORM_STATE_HPP
