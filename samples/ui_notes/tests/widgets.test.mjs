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

// The registry of widgets the Vite plugin makes from the C++ sources: run by ctest under node --test.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { widgetPaths, widgetRegistry } from '../quoll-vite-plugin.js';

test('the paths of widgets, with parentheses or braces, nested templates, and no comment', () => {
  const source = [
    'inline constexpr auto line = widget<chart_props>("./src/line.js", "canvas");',
    'constexpr auto bar = ui::widget<ns::props<int>>{ "./bar.js", "div" };',
    '// widget<old>("./old.js", "canvas");',
    '/* widget<older>("./older.js", "canvas"); */',
    'std::vector<widget<P>> charts; auto name = static_cast<std::string>("not a widget");',
    'constexpr auto map = widget<map_props>("leaflet-widget", "div");',
  ].join('\n');

  assert.deepEqual(widgetPaths(source), ['./src/line.js', './bar.js', 'leaflet-widget']);
});

test('strings holding what looks like a comment, before a widget or as its path', () => {
  const source = [
    'auto route = "/api/*";',
    'constexpr auto map = widget<map_props>("https://cdn.example/map.js", "div");',
    'constexpr auto c = widget<P>("./c.js", "canvas"); /* note */',
    'auto raw = R"(widget<X>("./not.js", "div"))";',
  ].join('\n');

  assert.deepEqual(widgetPaths(source), ['https://cdn.example/map.js', './c.js']);
});

test('a path is resolved from its source, a package left as it is', () => {
  const registry = widgetRegistry([
    ['/project/src/page.hpp', 'widget<P>("./widgets/line.js", "canvas") widget<Q>("leaflet-widget", "div")'],
    ['/project/notes/note.hpp', 'widget<R>("../shared/bar.js", "canvas")'],
  ]);

  assert.match(registry, /"\.\/widgets\/line\.js": \(\) => import\("\/project\/src\/widgets\/line\.js"\)/);
  assert.match(registry, /"leaflet-widget": \(\) => import\("leaflet-widget"\)/);
  assert.match(registry, /"\.\.\/shared\/bar\.js": \(\) => import\("\/project\/shared\/bar\.js"\)/);
});

test('one path naming two modules is refused', () => {
  const source = 'widget<P>("./line.js", "canvas")';

  assert.throws(
    () =>
      widgetRegistry([
        ['/project/a/page.hpp', source],
        ['/project/b/page.hpp', source],
      ]),
    /names \/project\/a\/line\.js and \/project\/b\/line\.js/,
  );
});
