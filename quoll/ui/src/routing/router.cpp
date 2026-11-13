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

#include <quoll/ui/routing/router.hpp>

#include <quoll/http/detail/percent.hpp>
#include <quoll/http/detail/route_template.hpp>
#include <quoll/ui/component/component.hpp>
#include <quoll/ui/component/hooks.hpp>
#include <quoll/ui/context/provided.hpp>
#include <quoll/ui/context/shared.hpp>
#include <quoll/ui/detail/app/app_core.hpp>
#include <quoll/ui/detail/component/instance.hpp>
#include <quoll/ui/detail/view/view_context.hpp>
#include <quoll/ui/view/decorators.hpp>
#include <quoll/ui/view/tags.hpp>

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quoll::ui {

namespace detail {

namespace {

constexpr std::size_t none = static_cast<std::size_t>(-1);

// The most redirects one address follows: a loop of them ends there, on the last.
constexpr int most_redirects = 8;

// Whether `segment` is ".", or ".." for `two`, as a browser reads them in a path, percent-encoded or not.
[[nodiscard]] bool is_dots(std::string_view segment, bool two) noexcept {
  auto const dot = [](std::string_view part) { return part == "." || part == "%2e" || part == "%2E"; };

  if (!two) {
    return dot(segment);
  }

  for (std::size_t first = 1; first <= 3 && first < segment.size(); ++first) {
    if (dot(segment.substr(0, first)) && dot(segment.substr(first))) {
      return true;
    }
  }

  return false;
}

// `path` in the one form an app's addresses have, as a browser keeps it: rooted, its "." and ".." segments resolved,
// no last '/' but for "/". What is pushed onto a history and what is matched are both of this form.
[[nodiscard]] std::string canonical_path(std::string_view path) {
  std::vector<std::string_view> segments;

  for (std::size_t start = path.empty() || path.front() != '/' ? 0 : 1; start <= path.size();) {
    auto const slash = path.find('/', start);
    auto const segment = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);

    if (is_dots(segment, true)) {
      if (!segments.empty()) {
        segments.pop_back();
      }
    } else if (!is_dots(segment, false)) {
      segments.push_back(segment);
    }

    if (slash == std::string_view::npos) {
      break;
    }

    start = slash + 1;
  }

  while (!segments.empty() && segments.back().empty()) {
    segments.pop_back();
  }

  std::string canonical;

  for (auto const segment : segments) {
    canonical += '/';
    canonical += segment;
  }

  return canonical.empty() ? std::string{"/"} : canonical;
}

// `where` as an address, its query after its path.
[[nodiscard]] std::string href_of(location const &where) {
  return where.query.empty() ? where.path : where.path + "?" + where.query;
}

// `href` split into its path, in its one form, and its query, its fragment dropped.
[[nodiscard]] location location_of(std::string_view href) {
  if (auto const hash = href.find('#'); hash != std::string_view::npos) {
    href = href.substr(0, hash);
  }

  location found;
  auto const question = href.find('?');

  found.path = canonical_path(href.substr(0, question));

  if (question != std::string_view::npos) {
    found.query = std::string{href.substr(question + 1)};
  }

  return found;
}

// The segments of `path`, percent-decoded, none for a path that does not decode.
[[nodiscard]] std::optional<std::vector<std::string>> segments_of(std::string_view path) {
  std::vector<std::string> segments;

  for (std::size_t start = 1;;) {
    auto const slash = path.find('/', start);
    auto decoded =
        http::detail::percent_decode(path.substr(start, slash == std::string_view::npos ? slash : slash - start));

    if (!decoded) {
      return std::nullopt;
    }

    segments.push_back(std::move(*decoded));

    if (slash == std::string_view::npos) {
      return segments;
    }

    start = slash + 1;
  }
}

// The pairs of `query`, percent-decoded, '+' a space as a form sends it: one that does not decode is left out.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> query_of(std::string_view query) {
  std::vector<std::pair<std::string, std::string>> pairs;

  while (!query.empty()) {
    auto const amp = query.find('&');
    auto pair = std::string{query.substr(0, amp)};

    query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);

    for (auto &character : pair) {
      character = character == '+' ? ' ' : character;
    }

    auto const equals = pair.find('=');
    auto key = http::detail::percent_decode(std::string_view{pair}.substr(0, equals));
    auto value = equals == std::string::npos ? std::optional<std::string>{std::string{}}
                                             : http::detail::percent_decode(std::string_view{pair}.substr(equals + 1));

    if (key && value && !key->empty()) {
      pairs.emplace_back(std::move(*key), std::move(*value));
    }
  }

  return pairs;
}

// Whether `segments` match the whole template `path`, its parameters then in `values`.
[[nodiscard]] bool matches(std::string_view path, std::vector<std::string> const &segments, route_values &values) {
  auto const parsed = http::detail::parse_template(path);
  std::size_t at = 0;

  for (auto const &segment : parsed.segments) {
    if (segment.kind == http::detail::segment_kind::rest) {
      if (at >= segments.size()) {
        return false;
      }

      std::string rest;

      // A separator before each segment but the first, an empty one included, so that "/a" stays "/a".
      for (auto const first = at; at < segments.size(); ++at) {
        if (at != first) {
          rest += '/';
        }

        rest += segments[at];
      }

      values.path.emplace_back(std::string{segment.text}, std::move(rest));
      return true;
    }

    if (at >= segments.size()) {
      return false;
    }

    if (segment.kind == http::detail::segment_kind::literal) {
      if (http::detail::decoded_literal(segment.text) != segments[at]) {
        return false;
      }
    } else {
      values.path.emplace_back(std::string{segment.text}, segments[at]);
    }

    ++at;
  }

  return at == segments.size();
}

// The routes from the top to the first leaf whose path matches `where` and whose pages take its values, or the
// not_found page's, or none.
[[nodiscard]] route_match match_of(std::span<route_entry const> routes, location const &where) {
  route_match found;
  auto const segments = segments_of(where.path);

  if (segments) {
    for (std::size_t index = 0; index < routes.size(); ++index) {
      auto const &route = routes[index];

      if (!route.leaf || route.kind == route_kind::not_found) {
        continue;
      }

      route_values values;

      if (!matches(route.path, *segments, values)) {
        continue;
      }

      values.query = query_of(where.query);

      bool suits = true;

      for (auto at = index; at != none; at = routes[at].parent) {
        suits = suits && (routes[at].accepts == nullptr || routes[at].accepts(values));
      }

      if (!suits) {
        continue;
      }

      for (auto at = index; at != none; at = routes[at].parent) {
        found.chain.insert(found.chain.begin(), at);
      }

      found.values = std::move(values);
      return found;
    }
  }

  for (std::size_t index = 0; index < routes.size(); ++index) {
    if (routes[index].kind == route_kind::not_found) {
      found.chain = {index};
      found.values.query = query_of(where.query);
      return found;
    }
  }

  return found;
}

// The address `target`, a whole template, with the values of the address it redirects from, and its query.
[[nodiscard]] std::string redirected(std::string_view target, route_values const &values, location const &from) {
  std::string href;

  for (auto const &segment : http::detail::parse_template(target).segments) {
    href += '/';

    if (segment.kind == http::detail::segment_kind::literal) {
      href += segment.text;
    } else if (auto const *const value = values.find(segment.text, true)) {
      href += http::detail::percent_encode(*value, segment.kind == http::detail::segment_kind::rest ? "/" : "");
    }
  }

  if (href.empty()) {
    href = "/";
  }

  if (!from.query.empty()) {
    href += '?';
    href += from.query;
  }

  return href;
}

struct outlet_props {
  std::size_t depth = 0;

  [[nodiscard]] bool operator==(outlet_props const &) const = default;
};

// Where an outlet shows the page of its depth, telling the outlets of that page's view theirs.
constexpr component<outlet_props> outlet_view = [](outlet_props const &given) {
  auto &nav = use_context<router>();
  auto &level = provide_context<outlet_level>();

  level->depth = given.depth;

  // Read, so that it shows again as the location changes.
  static_cast<void>(*nav->where);

  return nav->core.show(given.depth);
};

} // namespace

router_core::~router_core() {
  if (history_) {
    history_->unlisten(this);
  }
}

void router_core::start(
    std::span<route_entry const> routes, history_ref history, std::unique_ptr<memory_history> owned,
    state<location> &where
) {
  routes_ = routes;
  owned_ = std::move(owned);
  history_.emplace(history);
  where_ = &where;
  history_->listen(this, &router_core::changed);
  arrive(history_->current());
}

void router_core::go(std::string_view href, bool replace) {
  // Where it ends, its redirects followed: going to the address shown, or to one that redirects there, pushes nothing,
  // as a browser's link to the page it shows replaces its entry.
  auto where = resolved(href);
  auto const rooted = href_of(where);

  if (replace || where == **where_) {
    history_->replace(rooted);
  } else {
    history_->push(rooted);
  }

  set_location(std::move(where));
}

void router_core::back() const { history_->back(); }

void router_core::changed(void *self, std::string_view href) { static_cast<router_core *>(self)->arrive(href); }

location router_core::resolved(std::string_view href) const {
  auto where = location_of(href);

  for (int followed = 0; followed < most_redirects; ++followed) {
    auto const found = match_of(routes_, where);

    if (found.chain.empty() || routes_[found.chain.back()].kind != route_kind::redirect) {
      break;
    }

    where = location_of(redirected(routes_[found.chain.back()].target, found.values, where));
  }

  return where;
}

void router_core::arrive(std::string_view href) {
  auto where = resolved(href);

  // An address the history gave that redirects elsewhere: the history holds where it went.
  if (auto const rooted = href_of(where); rooted != href) {
    history_->replace(rooted);
  }

  set_location(std::move(where));
}

void router_core::set_location(location where) {
  if (**where_ != where) {
    where_->set(std::move(where));
  }
}

element router_core::show(std::size_t depth) {
  if (matched_at_ != **where_) {
    matched_ = match_of(routes_, **where_);
    matched_at_ = **where_;
  }

  if (depth >= matched_.chain.size()) {
    return node("div");
  }

  auto const &route = routes_[matched_.chain[depth]];

  return route.show == nullptr ? node("div") : route.show(matched_.values);
}

std::string router_core::href(component_table const &page, void const *props) const {
  for (auto const &route : routes_) {
    if (route.kind == route_kind::page && route.page() == &page) {
      return route.href(props);
    }
  }

  throw std::logic_error{"quoll::ui::router: no route shows this page"};
}

std::size_t outlet_depth() {
  auto const owner = current_target().owner.lock();

  if (!owner) {
    return 0;
  }

  auto const found = instance::provided_above(owner, async::detail::type_id_of<outlet_level>());

  return found.entry == nullptr ? 0 : static_cast<outlet_level *>(found.entry->object())->depth + 1;
}

router &router_in_view() {
  auto const target = current_target();
  auto *const core = target.app ? target.app->core : nullptr;
  auto *const entry = core == nullptr ? nullptr : core->context(async::detail::type_id_of<router>());

  if (entry == nullptr) {
    throw std::logic_error{"quoll::ui: a link is in a view of an app given routing()"};
  }

  return *static_cast<router *>(entry->object());
}

element linked(router &nav, std::string href, element content) {
  // What a click on it does, unless it opens elsewhere, as a click with a key held: the page goes there.
  auto const go = [target = &nav, href] { target->go(href); };

  return std::move(content) | attribute("href", href) | attribute("data-quoll-link", "") | on_click(go);
}

} // namespace detail

element outlet() { return detail::outlet_view({.depth = detail::outlet_depth()}); }

element link_to(std::string_view href, std::initializer_list<detail::child> children) {
  return detail::linked(detail::router_in_view(), std::string{href}, node("a", children));
}

element link_to(component_base<void> const &page, std::initializer_list<detail::child> children) {
  auto &nav = detail::router_in_view();

  return detail::linked(nav, nav.href(page), node("a", children));
}

} // namespace quoll::ui
