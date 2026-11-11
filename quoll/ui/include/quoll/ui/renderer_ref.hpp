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

#ifndef QUOLL_UI_RENDERER_REF_HPP
#define QUOLL_UI_RENDERER_REF_HPP

#include <quoll/async/detail/type_id.hpp>
#include <quoll/ui/node_id.hpp>
#include <quoll/ui/renderer.hpp>
#include <quoll/ui/widget_renderer.hpp>

#include <concepts>
#include <string_view>
#include <type_traits>

namespace quoll::ui {

/**
 * @brief Non-owning, type-erased reference to a Renderer; models Renderer itself. What lets what shows an interface be
 * compiled once, whatever shows it.
 *
 * Trivially copyable and never allocates. The referenced renderer must outlive it.
 *
 * Thread safety: same as the referenced renderer.
 */
class renderer_ref {
public:
  // The exclusion comes first: it stops Renderer<renderer_ref> from checking this constructor against itself.
  template <class R>
    requires(!std::same_as<R, renderer_ref> && !std::is_const_v<R> && Renderer<R>)
  explicit renderer_ref(R &renderer) noexcept : renderer_(&renderer), vtable_(&vtable_for<R>) {}

  /** @brief The referenced renderer, if it is an `R`. */
  template <class R>
  [[nodiscard]] R *target() const noexcept {
    return vtable_->type == async::detail::type_id_of<R>() ? static_cast<R *>(renderer_) : nullptr;
  }

  void create_element(node_id id, std::string_view tag) const { vtable_->create_element(renderer_, id, tag); }

  void create_text(node_id id, std::string_view content) const { vtable_->create_text(renderer_, id, content); }

  void set_text(node_id id, std::string_view content) const { vtable_->set_text(renderer_, id, content); }

  void set_class(node_id id, std::string_view classes) const { vtable_->set_class(renderer_, id, classes); }

  void set_attribute(node_id id, std::string_view name, std::string_view value) const {
    vtable_->set_attribute(renderer_, id, name, value);
  }

  void remove_attribute(node_id id, std::string_view name) const { vtable_->remove_attribute(renderer_, id, name); }

  void set_value(node_id id, std::string_view value) const { vtable_->set_value(renderer_, id, value); }

  void listen(node_id id, std::string_view name) const { vtable_->listen(renderer_, id, name); }

  void unlisten(node_id id, std::string_view name) const { vtable_->unlisten(renderer_, id, name); }

  void insert(node_id parent, node_id child, node_id before) const {
    vtable_->insert(renderer_, parent, child, before);
  }

  void remove(node_id id) const { vtable_->remove(renderer_, id); }

  /** @brief Those of a WidgetRenderer: throw `std::logic_error` for one without them, which shows no widget. */
  void mount_widget(node_id id, std::string_view module, std::string_view props) const {
    widgets()->mount_widget(renderer_, id, module, props);
  }

  void update_widget(node_id id, std::string_view props) const { widgets()->update_widget(renderer_, id, props); }

  void unmount_widget(node_id id) const { widgets()->unmount_widget(renderer_, id); }

private:
  struct widget_table {
    void (*mount_widget)(void *, node_id, std::string_view, std::string_view);
    void (*update_widget)(void *, node_id, std::string_view);
    void (*unmount_widget)(void *, node_id);
  };

  struct vtable {
    async::detail::type_id type;
    void (*create_element)(void *, node_id, std::string_view);
    void (*create_text)(void *, node_id, std::string_view);
    void (*set_text)(void *, node_id, std::string_view);
    void (*set_class)(void *, node_id, std::string_view);
    void (*set_attribute)(void *, node_id, std::string_view, std::string_view);
    void (*remove_attribute)(void *, node_id, std::string_view);
    void (*set_value)(void *, node_id, std::string_view);
    void (*listen)(void *, node_id, std::string_view);
    void (*unlisten)(void *, node_id, std::string_view);
    void (*insert)(void *, node_id, node_id, node_id);
    void (*remove)(void *, node_id);
    // None for a renderer that shows no widget.
    widget_table const *widgets;
  };

  template <class R>
  static constexpr widget_table widgets_for{
      [](void *r, node_id id, std::string_view module, std::string_view props) {
        static_cast<R *>(r)->mount_widget(id, module, props);
      },
      [](void *r, node_id id, std::string_view props) { static_cast<R *>(r)->update_widget(id, props); },
      [](void *r, node_id id) { static_cast<R *>(r)->unmount_widget(id); },
  };

  template <class R>
  [[nodiscard]] static consteval widget_table const *widgets_of() noexcept {
    if constexpr (WidgetRenderer<R>) {
      return &widgets_for<R>;
    } else {
      return nullptr;
    }
  }

  [[nodiscard]] widget_table const *widgets() const;

  template <class R>
  static constexpr vtable vtable_for{
      async::detail::type_id_of<R>(),
      [](void *r, node_id id, std::string_view tag) { static_cast<R *>(r)->create_element(id, tag); },
      [](void *r, node_id id, std::string_view content) { static_cast<R *>(r)->create_text(id, content); },
      [](void *r, node_id id, std::string_view content) { static_cast<R *>(r)->set_text(id, content); },
      [](void *r, node_id id, std::string_view classes) { static_cast<R *>(r)->set_class(id, classes); },
      [](void *r, node_id id, std::string_view name, std::string_view value) {
        static_cast<R *>(r)->set_attribute(id, name, value);
      },
      [](void *r, node_id id, std::string_view name) { static_cast<R *>(r)->remove_attribute(id, name); },
      [](void *r, node_id id, std::string_view value) { static_cast<R *>(r)->set_value(id, value); },
      [](void *r, node_id id, std::string_view name) { static_cast<R *>(r)->listen(id, name); },
      [](void *r, node_id id, std::string_view name) { static_cast<R *>(r)->unlisten(id, name); },
      [](void *r, node_id parent, node_id child, node_id before) {
        static_cast<R *>(r)->insert(parent, child, before);
      },
      [](void *r, node_id id) { static_cast<R *>(r)->remove(id); },
      widgets_of<R>(),
  };

  void *renderer_;
  vtable const *vtable_;
};

static_assert(Renderer<renderer_ref>);
static_assert(WidgetRenderer<renderer_ref>);

} // namespace quoll::ui

#endif // QUOLL_UI_RENDERER_REF_HPP
