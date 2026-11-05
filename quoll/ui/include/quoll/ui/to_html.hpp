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

#ifndef QUOLL_UI_TO_HTML_HPP
#define QUOLL_UI_TO_HTML_HPP

#include <quoll/ui/element.hpp>

#include <string>

namespace quoll::ui {

/**
 * @brief An interface as HTML, for a page generated ahead of time or sent by a server: `to_html(home{}.view())`.
 *
 * A whole page starts with what it writes around it: `<!doctype html>` first, or the browser reads it in quirks mode,
 * and
 * `<meta charset="utf-8">`, as what is escaped here is only safe read as UTF-8.
 *
 * It writes the tree as it is given: a nesting a parser rearranges, such as a `div` in a `p` or a `tr` directly in a
 * `table`, shows as in a page written by hand, not as in the DOM. Handlers do nothing here: this is the page as it
 * first shows, without the WASM that would make it live. A `value` on a `select` selects nothing.
 *
 * Text and attribute values are escaped, so that what a user wrote cannot become markup. In `script` and `style`, where
 * escaping would change the meaning, text goes as it is: it must come from the application, or be refused here.
 * Escaping cannot make an attribute safe by what it means: `href` with a `javascript:` URL, or a handler such as
 * `onclick`, are the application's to refuse. The value of an `input` of type `password` is left out: the page may be
 * stored.
 *
 * @throws std::invalid_argument for a tag or an attribute name that HTML cannot hold, an element a parser reads by
 * rules of its own (`plaintext`, `xmp`, `noembed`, `noframes`, `frame`, `frameset`), an attribute given twice in any
 * case
 * (`class` and `value` come from their decorators), children in an element that has none (`br`, `img`, `input`,
 * `iframe`...), an element in one that holds only text (`script`, `style`, `textarea`, `title`), text in a `script` or
 * `style` that would end it early (`</`, `<!--`) or that sits inside `svg`, `math` or `select`.
 */
[[nodiscard]] std::string to_html(element const &view);

} // namespace quoll::ui

#endif // QUOLL_UI_TO_HTML_HPP
