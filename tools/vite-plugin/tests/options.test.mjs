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

import assert from 'node:assert/strict';
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';

import { developmentOptions, expandSources, inSources, quoll, within } from '../quoll-vite-plugin.js';

test('a development build has line tables by default, and the debug information asked for', () => {
  assert.deepEqual(developmentOptions(), [
    '-DCMAKE_BUILD_TYPE=Debug',
    '-DCMAKE_C_FLAGS_DEBUG=-gline-tables-only',
    '-DCMAKE_CXX_FLAGS_DEBUG=-gline-tables-only',
    '-DQUOLL_UI_HOT=ON',
  ]);
  assert.ok(developmentOptions('full').includes('-DCMAKE_CXX_FLAGS_DEBUG=-g'));
  assert.ok(developmentOptions('none').includes('-DCMAKE_CXX_FLAGS_DEBUG=-g0'));
});

test('another debug, or one inherited from Object, is refused as the plugin is made', () => {
  assert.throws(() => developmentOptions('toString'), /debug is one of lines, full, none, not toString/);
  assert.throws(() => quoll({ target: 't', sources: [], debug: 'some' }), /not some/);
});

test('the build directories are left out of the watch, as paths, brackets and parentheses included', () => {
  const ignored = within(['/code (old)/web/build', '/code [x]/web/build-release']);

  assert.ok(ignored('/code (old)/web/build'));
  assert.ok(ignored('/code (old)/web/build/quoll/cmake_install.cmake.tmp1'));
  assert.ok(ignored('/code [x]/web/build-release/a.wasm'));
  assert.ok(!ignored('/code (old)/web/build-native/a.o'));
  assert.ok(!ignored('/code (old)/web/components/page.hpp'));
});

test('the plugin hands Vite that test for its own build directories', () => {
  const { ignored } = quoll({ target: 't', sources: [], buildDir: 'out' }).config().server.watch;

  assert.ok(ignored(`${process.cwd()}/out/CMakeCache.txt`));
  assert.ok(ignored(`${process.cwd()}/out-release/a.js`));
  assert.ok(!ignored(`${process.cwd()}/src/app.js`));
});

test('a directory of the sources gives its C++ files at any depth, in order, a file is taken as it is', () => {
  const base = mkdtempSync(join(tmpdir(), 'quoll-sources-'));

  try {
    mkdirSync(join(base, 'src', 'components'), { recursive: true });
    writeFileSync(join(base, 'src', 'main.cpp'), '');
    writeFileSync(join(base, 'src', 'main.js'), '');
    writeFileSync(join(base, 'src', 'components', 'row.hpp'), '');
    writeFileSync(join(base, 'src', 'components', 'row.cpp'), '');
    writeFileSync(join(base, 'api.hpp'), '');

    mkdirSync(join(base, 'src', 'build', 'CMakeFiles'), { recursive: true });
    mkdirSync(join(base, 'src', 'node_modules', 'native'), { recursive: true });
    writeFileSync(join(base, 'src', 'build', 'CMakeFiles', 'id.cpp'), '');
    writeFileSync(join(base, 'src', 'node_modules', 'native', 'addon.h'), '');

    const builds = within([join(base, 'src', 'build')]);

    assert.deepEqual(expandSources(base, ['src', 'api.hpp', 'gone.hpp'], builds), [
      join(base, 'src', 'components', 'row.cpp'),
      join(base, 'src', 'components', 'row.hpp'),
      join(base, 'src', 'main.cpp'),
      join(base, 'api.hpp'),
      join(base, 'gone.hpp'),
    ]);
  } finally {
    rmSync(base, { recursive: true, force: true });
  }
});

test('a file made in a folder of the sources is one of them, but in a build directory or a node_modules', () => {
  const builds = within(['/web/build']);

  assert.ok(inSources('/web/src/components/card.cpp', ['/web/src'], builds));
  assert.ok(inSources('/web/src/components/card.hpp', ['/web/src'], builds));
  assert.ok(!inSources('/web/src/components/card.js', ['/web/src'], builds));
  assert.ok(!inSources('/web/other/card.cpp', ['/web/src'], builds));
  assert.ok(!inSources('/web/build/CMakeFiles/id.cpp', ['/web'], builds));
  assert.ok(!inSources('/web/src/node_modules/native/addon.h', ['/web/src'], builds));
});
