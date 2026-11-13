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

// The plugin's dev server, on a fake of Vite's and a build that always succeeds: what a source made, saved or removed
// makes it do. Run by ctest under node --test.

import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { chmodSync, mkdirSync, mkdtempSync, renameSync, rmSync, symlinkSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { delimiter, join } from 'node:path';
import { test } from 'node:test';

import { quoll } from '../quoll-vite-plugin.js';

// What the plugin uses of Vite's dev server, its messages to the page kept, and a promise of the next reload.
function fakeServer() {
  const watcher = new EventEmitter();
  const server = {
    watched: [],
    messages: [],
    invalidated: 0,
    reloaded: null,
    watcher,
    config: { logger: { info() {}, warn() {}, error() {} } },
    middlewares: { use() {} },
    moduleGraph: {
      getModuleById: () => ({}),
      invalidateModule: () => (server.invalidated += 1),
    },
    ws: {
      on() {},
      send: (message) => {
        server.messages.push(message);

        if (message.type === 'full-reload') {
          server.reloaded?.();
        }
      },
    },
  };

  watcher.add = (paths) => server.watched.push(...paths);
  return server;
}

// The next reload the events `act` fires bring, or a failure, rather than a test waiting for good.
function reload(server, act) {
  const reloaded = new Promise((done, failed) => {
    const timer = setTimeout(() => failed(new Error('no reload')), 5000);

    server.reloaded = () => {
      clearTimeout(timer);
      done();
    };
  });

  act();
  return reloaded;
}

test('a source made, saved or removed rebuilds the page, with its widgets, its texts alone are sent, and what is no source does nothing', async () => {
  const base = mkdtempSync(join(tmpdir(), 'quoll-watcher-'));
  const bin = join(base, 'bin');
  const cwd = process.cwd();
  const path = process.env.PATH;

  try {
    // A build that succeeds at once: emcmake and cmake, first on the path.
    mkdirSync(bin);

    for (const tool of ['emcmake', 'cmake']) {
      writeFileSync(join(bin, tool), '#!/bin/sh\nexit 0\n');
      chmodSync(join(bin, tool), 0o755);
    }

    process.env.PATH = `${bin}${delimiter}${path}`;
    mkdirSync(join(base, 'src', 'components'), { recursive: true });
    mkdirSync(join(base, 'src', 'node_modules'), { recursive: true });
    writeFileSync(join(base, 'src', 'main.cpp'), 'int main() {}\n');
    writeFileSync(join(base, 'shared.inl'), '');
    // A component linked from elsewhere, as one shared between pages.
    mkdirSync(join(base, 'shared'));
    writeFileSync(join(base, 'shared', 'map.cpp'), 'constexpr auto map = widget<props>("./map.js", "div");\n');
    symlinkSync(join(base, 'shared', 'map.cpp'), join(base, 'src', 'components', 'map.cpp'));
    process.chdir(base);

    const plugin = quoll({ target: 'page', sources: ['src', 'shared.inl'] });
    const server = fakeServer();
    const registry = () => plugin.load('\0virtual:quoll-widgets');
    const reloads = () => server.messages.filter((message) => message.type === 'full-reload').length;

    await plugin.configureServer(server);

    assert.deepEqual(server.watched, [join(base, 'src'), join(base, 'shared.inl')]);
    assert.equal(reloads(), 1);
    assert.match(registry(), /map\.js/);

    // A component made, with a widget: the page rebuilds, and the registry, made again, has it.
    const chart = join(base, 'src', 'components', 'chart.cpp');

    writeFileSync(chart, 'constexpr auto chart = widget<props>("./chart.js", "canvas");\n');
    server.invalidated = 0;
    await reload(server, () => server.watcher.emit('add', chart));
    assert.equal(reloads(), 2);
    assert.ok(server.invalidated > 0);
    assert.match(registry(), /chart\.js/);

    // Its text changed: sent to the page, no rebuild.
    const view = join(base, 'src', 'components', 'view.cpp');

    writeFileSync(view, 'auto view() { return text("before"); }\n');
    await reload(server, () => server.watcher.emit('add', view));

    const literals = () => server.messages.filter((message) => message.event === 'quoll:literal');

    writeFileSync(view, 'auto view() { return text("after"); }\n');
    await server.watcher.listeners('change')[0](view);
    assert.equal(reloads(), 3);
    assert.equal(literals().at(-1).data.value, 'after');

    // Saved as some editors do, a new file in place of the old, told as a removal and an addition: a change too.
    writeFileSync(`${view}.tmp`, 'auto view() { return text("again"); }\n');
    server.watcher.emit('unlink', view);
    renameSync(`${view}.tmp`, view);
    await server.watcher.listeners('add')[0](view);
    await new Promise((done) => setTimeout(done, 200));
    assert.equal(reloads(), 3);
    assert.equal(literals().at(-1).data.value, 'again');

    // The file named, saved, whatever its extension.
    writeFileSync(join(base, 'shared.inl'), 'inline int shared = 0;\n');
    await reload(server, () => server.watcher.emit('change', join(base, 'shared.inl')));
    assert.equal(reloads(), 4);

    // The component removed: the page rebuilds without it.
    rmSync(chart);
    server.invalidated = 0;
    await reload(server, () => server.watcher.emit('unlink', chart));
    assert.equal(reloads(), 5);
    assert.ok(server.invalidated > 0);
    assert.doesNotMatch(registry(), /chart\.js/);

    // What is no source: no rebuild, which would reload the page.
    const before = server.messages.length;

    writeFileSync(join(base, 'src', 'node_modules', 'addon.h'), '');
    server.watcher.emit('add', join(base, 'src', 'node_modules', 'addon.h'));
    server.watcher.emit('add', join(base, 'src', 'notes.txt'));
    server.watcher.emit('unlink', join(base, 'other.cpp'));
    server.watcher.emit('add', join(base, 'build', 'CMakeFiles', 'id.cpp'));
    await new Promise((done) => setTimeout(done, 200));
    assert.equal(server.messages.length, before);
  } finally {
    process.chdir(cwd);
    process.env.PATH = path;
    rmSync(base, { recursive: true, force: true });
  }
});
