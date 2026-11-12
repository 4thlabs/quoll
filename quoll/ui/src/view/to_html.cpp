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

#include <quoll/ui/view/to_html.hpp>

#include <quoll/ui/detail/view/expand.hpp>
#include <quoll/ui/view/element.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace quoll::ui {

namespace {

[[noreturn]] void refuse(std::string const &what) { throw std::invalid_argument{"quoll::ui::to_html: " + what}; }

// ASCII only, as HTML's names are, and whatever the locale: std::tolower changes with it.
[[nodiscard]] constexpr char lower(char character) noexcept {
  return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
}

[[nodiscard]] constexpr bool is_letter(char character) noexcept {
  return lower(character) >= 'a' && lower(character) <= 'z';
}

[[nodiscard]] constexpr bool is_digit(char character) noexcept { return character >= '0' && character <= '9'; }

[[nodiscard]] constexpr bool same_name(std::string_view first, std::string_view second) noexcept {
  return std::ranges::equal(first, second, {}, lower, lower);
}

template <std::size_t Size>
[[nodiscard]] constexpr bool is_one_of(std::array<std::string_view, Size> const &names, std::string_view tag) noexcept {
  return std::ranges::any_of(names, [tag](std::string_view name) { return same_name(name, tag); });
}

// HTML's elements without an end tag, which cannot hold anything. A parser reads `image` as `img`, outside svg, whose
// own `image` holds nothing either.
inline constexpr std::array<std::string_view, 18> void_tags{"area",  "base", "basefont", "bgsound", "br",    "col",
                                                            "embed", "hr",   "image",    "img",     "input", "keygen",
                                                            "link",  "meta", "param",    "source",  "track", "wbr"};

// What parsers read by rules of their own, and nothing a page needs: `plaintext` swallows the rest of the page, the
// others hold unparsed text or depend on what surrounds them.
// `noscript` is kept, which real pages need: what it holds is escaped, or refused like anywhere else.
inline constexpr std::array<std::string_view, 6> refused_tags{"frame",    "frameset",  "noembed",
                                                              "noframes", "plaintext", "xmp"};

// A parser does not read a `script` or a `style` as raw text inside these, or may not, so that text left as it is there
// would be markup. The rules are too many to follow, and differ between parsers: such text is refused.
inline constexpr std::array<std::string_view, 3> raw_text_refused_in{"math", "select", "svg"};

// A parser drops a newline right after the start tag of these: one more keeps the one that was meant.
inline constexpr std::array<std::string_view, 3> newline_dropped_in{"listing", "pre", "textarea"};

// A letter, then what a custom element or an SVG name holds. Stricter than what a parser reads, as the DOM's
// createElement is for most of it: anything else would be another tag or attribute once written.
void check_tag(std::string_view tag) {
  auto const valid_rest = [](char character) {
    return is_letter(character) || is_digit(character) || character == '-' || character == '_' || character == ':' ||
           character == '.';
  };

  if (tag.empty() || !is_letter(tag.front()) || !std::ranges::all_of(tag, valid_rest)) {
    refuse("invalid tag " + std::string{tag});
  }

  if (is_one_of(refused_tags, tag)) {
    refuse("a " + std::string{tag} + " element, which a parser reads by rules of its own");
  }
}

// What the HTML standard refuses in an attribute name.
void check_attribute_name(std::string_view name) {
  auto const invalid = [](char character) {
    auto const byte = static_cast<unsigned char>(character);

    return byte <= 0x20 || byte == 0x7F || character == '"' || character == '\'' || character == '>' ||
           character == '/' || character == '=' || character == '<';
  };

  if (name.empty() || std::ranges::any_of(name, invalid)) {
    refuse("invalid attribute name " + std::string{name});
  }
}

void append_escaped(std::string_view text, std::string &out) {
  for (auto const character : text) {
    switch (character) {
    case '&':
      out += "&amp;";
      break;
    case '<':
      out += "&lt;";
      break;
    case '>':
      out += "&gt;";
      break;
    case '"':
      out += "&quot;";
      break;
    case '\'':
      out += "&#39;";
      break;
    // A parser reads a carriage return as a newline, unless it comes as a reference.
    case '\r':
      out += "&#13;";
      break;
    default:
      out += character;
    }
  }
}

void append_attribute(std::string_view name, std::string_view value, std::string &out) {
  out += ' ';
  out += name;
  out += "=\"";
  append_escaped(value, out);
  out += '"';
}

// A generated page may be stored and served to anyone: a password stays out of it, as it does in the DOM's markup.
[[nodiscard]] bool holds_a_secret(element const &shown) noexcept {
  return std::ranges::any_of(shown.attributes, [](auto const &entry) {
    return same_name(entry.first, "type") && same_name(entry.second, "password");
  });
}

// A parser keeps the first of two attributes of a name, in any case, where the DOM ends on the last one set.
void write_attributes(element const &shown, bool value_is_attribute, std::string &out) {
  auto const given_by_decorator = [&](std::string_view name) {
    return (!shown.classes.empty() && same_name(name, "class")) ||
           (value_is_attribute && shown.value && same_name(name, "value")) ||
           (shown.checked && same_name(name, "checked"));
  };

  if (!shown.classes.empty()) {
    append_attribute("class", shown.classes, out);
  }

  if (value_is_attribute && shown.value) {
    append_attribute("value", *shown.value, out);
  }

  // The attribute a page parsed starts its property with.
  if (shown.checked.value_or(false)) {
    append_attribute("checked", "", out);
  }

  for (auto entry = shown.attributes.begin(); entry != shown.attributes.end(); ++entry) {
    auto const &name = entry->first;

    check_attribute_name(name);

    if (given_by_decorator(name) || std::any_of(std::next(entry), shown.attributes.end(), [&name](auto const &other) {
          return same_name(name, other.first);
        })) {
      refuse("attribute given twice: " + name);
    }

    append_attribute(name, entry->second, out);
  }
}

// All the text of an element that can hold nothing else: split into nodes, it is written as one.
[[nodiscard]] std::string text_of(element const &shown) {
  std::string content;

  for (auto const &child : shown.children) {
    if (child.type != element::kind::text) {
      refuse("an element in a " + shown.tag + ", which holds only text");
    }

    content += child.content;
  }

  return content;
}

// The first text of an element, past any empty one, if that comes first.
[[nodiscard]] std::string_view leading_text(element const &shown) noexcept {
  for (auto const &child : shown.children) {
    if (child.type != element::kind::text) {
      return {};
    }

    if (!child.content.empty()) {
      return child.content;
    }
  }

  return {};
}

void keep_first_newline(std::string_view tag, std::string_view text, std::string &out) {
  if (is_one_of(newline_dropped_in, tag) && text.starts_with('\n')) {
    out += '\n';
  }
}

// What is around an element, for what a parser would read differently there.
struct surroundings {
  bool raw_text_refused = false;
};

void write(element const &shown, surroundings around, std::string &out);

void write_raw_text(element const &shown, surroundings around, std::string &out) {
  auto const content = text_of(shown);

  if (content.empty()) {
    return;
  }

  if (around.raw_text_refused) {
    refuse("text in a " + shown.tag + " inside svg, math or select, where a parser would read it as markup");
  }

  // Nothing in it may close its element, or start what would keep the end tag from closing it.
  if (content.contains("</") || content.contains("<!--")) {
    refuse("text that would end a " + shown.tag + " early");
  }

  out += content;
}

void write_content(element const &shown, surroundings around, std::string &out) {
  if (same_name(shown.tag, "script") || same_name(shown.tag, "style")) {
    write_raw_text(shown, around, out);
    return;
  }

  if (same_name(shown.tag, "iframe")) {
    if (!shown.children.empty()) {
      refuse("children in an iframe, which a parser reads as unparsed text");
    }

    return;
  }

  // Parsed for entities and nothing else: an element in them would be text. A textarea's value shows in place of its
  // text.
  if (same_name(shown.tag, "textarea") || same_name(shown.tag, "title")) {
    auto const text = text_of(shown);
    std::string_view const content = shown.value && same_name(shown.tag, "textarea") ? *shown.value : text;

    keep_first_newline(shown.tag, content, out);
    append_escaped(content, out);
    return;
  }

  keep_first_newline(shown.tag, leading_text(shown), out);

  around.raw_text_refused = around.raw_text_refused || is_one_of(raw_text_refused_in, shown.tag);

  for (auto const &child : shown.children) {
    write(child, around, out);
  }
}

void write(element const &shown, surroundings around, std::string &out) {
  if (shown.type == element::kind::text) {
    append_escaped(shown.content, out);
    return;
  }

  // Stands for its children.
  if (shown.type == element::kind::fragment) {
    for (auto const &child : shown.children) {
      write(child, around, out);
    }

    return;
  }

  check_tag(shown.tag);

  // Its node is its module's, mounted once the page is live.
  if (shown.type == element::kind::widget && !shown.children.empty()) {
    throw std::logic_error{"quoll::ui: a widget's node is its module's: it takes no children"};
  }

  out += '<';
  out += shown.tag;
  write_attributes(shown, !same_name(shown.tag, "textarea") && !holds_a_secret(shown), out);

  // The slash means nothing to a void element in HTML, and ends it where a parser reads svg or math, whose `link` or
  // `source` is an element like any other.
  if (is_one_of(void_tags, shown.tag)) {
    if (!shown.children.empty()) {
      refuse("children in a " + shown.tag + ", which holds none");
    }

    out += "/>";
    return;
  }

  out += '>';
  write_content(shown, around, out);
  out += "</";
  out += shown.tag;
  out += '>';
}

} // namespace

std::string to_html(element const &view) {
  std::string out;

  if (!detail::has_component(view)) {
    write(view, {}, out);
    return out;
  }

  // Its components, with their views as they first show: made for this page only, and gone with it.
  auto expanded = view;
  std::vector<std::shared_ptr<detail::instance>> components;
  detail::expansion made;

  detail::expand(expanded, components, made);
  write(expanded, {}, out);
  return out;
}

} // namespace quoll::ui
