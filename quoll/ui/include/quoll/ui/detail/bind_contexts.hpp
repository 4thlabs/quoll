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

#ifndef QUOLL_UI_DETAIL_BIND_CONTEXTS_HPP
#define QUOLL_UI_DETAIL_BIND_CONTEXTS_HPP

#include <quoll/async/detail/type_id.hpp>
#include <quoll/ui/detail/context_entry.hpp>
#include <quoll/ui/detail/context_member.hpp>
#include <quoll/ui/detail/for_each_member.hpp>

#include <glaze/reflection/to_tuple.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace quoll::ui::detail {

// What a component or a context told of a context it uses and does not find.
inline constexpr char const *missing_context =
    "quoll::ui::shared: no context of this type was found: the app was given none, or, for a context using it, none "
    "before it";

// What a table holds to tell, before making a `C`, whether the contexts its members use are there: none for one that
// uses none, and true with no finder, for a view written once.
template <class C>
[[nodiscard]] constexpr auto finds_of() noexcept -> bool (*)(context_finder const &);

template <class C>
[[nodiscard]] constexpr auto finds_of() noexcept -> bool (*)(context_finder const &) {
  if constexpr (any_member<C, is_context_user>) {
    return [](context_finder const &finder) {
      using members = decltype(glz::to_tie(std::declval<C &>()));

      if (finder.find == nullptr) {
        return true;
      }

      return [&finder]<std::size_t... Index>(std::index_sequence<Index...>) {
        auto const found = [&finder]<class M>(std::type_identity<M>) {
          if constexpr (ContextMember<M>) {
            return finder(async::detail::type_id_of<typename M::element_type>()).entry != nullptr;
          } else if constexpr (ProvidedMember<M>) {
            // What it provides finds its own as its component does.
            constexpr auto inner = finds_of<typename M::provided_type>();

            return inner == nullptr || inner(finder);
          } else {
            return true;
          }
        };

        return (
            found(std::type_identity<std::remove_cvref_t<decltype(glz::get<Index>(std::declval<members &>()))>>{}) &&
            ...);
      }(std::make_index_sequence<glz::tuple_size_v<members>>{});
    };
  } else {
    return nullptr;
  }
}

// What a table holds to find the contexts the members of `C` use: none for one that uses none.
template <class C>
[[nodiscard]] constexpr auto binds_of() noexcept -> void (*)(void *, context_finder const &) {
  if constexpr (any_member<C, is_context_user>) {
    return [](void *object, context_finder const &finder) {
      for_each_member(*static_cast<C *>(object), [&finder](auto &member) {
        using member_type = std::remove_cvref_t<decltype(member)>;

        if constexpr (ContextMember<member_type> || ProvidedMember<member_type>) {
          member.bind(finder);
        }
      });
    };
  } else {
    return nullptr;
  }
}

// What a table holds to count the changes of the contexts a context `C` uses, which count in its own: none for one that
// uses none.
template <class C>
[[nodiscard]] constexpr auto contexts_version_of() noexcept -> std::uint64_t (*)(void *) {
  if constexpr (any_member<C, is_context_member>) {
    return [](void *object) {
      std::uint64_t total = 0;

      for_each_member(*static_cast<C *>(object), [&total](auto &member) {
        if constexpr (ContextMember<std::remove_cvref_t<decltype(member)>>) {
          total += member.context_version();
        }
      });

      return total;
    };
  } else {
    return nullptr;
  }
}

// What a table holds to mark what the members of `C` see of their contexts, as its view runs: none for one that uses
// none.
template <class C>
[[nodiscard]] constexpr auto marks_of() noexcept -> void (*)(void *) {
  if constexpr (any_member<C, is_context_member>) {
    return [](void *object) {
      for_each_member(*static_cast<C *>(object), [](auto &member) {
        if constexpr (ContextMember<std::remove_cvref_t<decltype(member)>>) {
          context_access::mark(member);
        }
      });
    };
  } else {
    return nullptr;
  }
}

// What a table holds to keep what the members of `C` marked, once its view shows: none for one that uses none.
template <class C>
[[nodiscard]] constexpr auto commits_of() noexcept -> void (*)(void *) {
  if constexpr (any_member<C, is_context_member>) {
    return [](void *object) {
      for_each_member(*static_cast<C *>(object), [](auto &member) {
        if constexpr (ContextMember<std::remove_cvref_t<decltype(member)>>) {
          context_access::commit(member);
        }
      });
    };
  } else {
    return nullptr;
  }
}

// What a table holds to tell whether a context the members of `C` read changed since their last view that showed: none
// for one that uses none.
template <class C>
[[nodiscard]] constexpr auto changes_of() noexcept -> bool (*)(void *) {
  if constexpr (any_member<C, is_context_member>) {
    return [](void *object) {
      bool changed = false;

      for_each_member(*static_cast<C *>(object), [&changed](auto &member) {
        if constexpr (ContextMember<std::remove_cvref_t<decltype(member)>>) {
          changed = changed || context_access::changed(member);
        }
      });

      return changed;
    };
  } else {
    return nullptr;
  }
}

// ProvidedMember, as a trait for any_member().
template <class T>
struct is_provided_member : std::bool_constant<ProvidedMember<T>> {};

// What a table holds to give the context of a type the members of `C` provide: none for one that provides none.
template <class C>
[[nodiscard]] constexpr auto provides_of() noexcept -> context_entry *(*)(void *, async::detail::type_id) {
  if constexpr (any_member<C, is_provided_member>) {
    return [](void *object, async::detail::type_id type) -> context_entry * {
      context_entry *found = nullptr;

      for_each_member(*static_cast<C *>(object), [&found, type](auto &member) {
        using member_type = std::remove_cvref_t<decltype(member)>;

        if constexpr (ProvidedMember<member_type>) {
          if (found == nullptr && async::detail::type_id_of<typename member_type::provided_type>() == type) {
            found = &member.entry();
          }
        }
      });

      return found;
    };
  } else {
    return nullptr;
  }
}

} // namespace quoll::ui::detail

#endif // QUOLL_UI_DETAIL_BIND_CONTEXTS_HPP
