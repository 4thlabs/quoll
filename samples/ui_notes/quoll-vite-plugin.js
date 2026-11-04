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


// A prototype of the planned `quoll/vite` plugin: builds a program of C++ with CMake, serves it, and reloads it.
//
// Three levels, from the cheapest:
// 1. CSS: Vite and Tailwind's own plugin, which reads the C++ for class names.
// 2. A text or a class list changed in the source: sent to the running program, which keeps its state. No compilation.
// 3. Anything else: a rebuild, then the page reloads. A compilation error shows in Vite's overlay.

import { spawn } from 'node:child_process';
import { createReadStream, existsSync, readFileSync } from 'node:fs';
import { dirname, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');

// What takes one literal that hot reload can replace: tw(), text() and the tags of quoll/ui/tags.hpp.
const replaceable = new Set([
  'tw', 'text', 'a', 'article', 'button', 'div', 'footer', 'form', 'h1', 'h2', 'h3', 'header', 'label', 'li', 'main_',
  'nav', 'ol', 'p', 'section', 'span', 'ul',
]);

// Splits C++ into its code, with comments gone and strings blanked, and its string literals: where they are, and whether
// hot reload can replace them. Two sources with the same code differ only in comments and literals.
export function scan(source) {
  let code = '';
  const literals = [];
  let line = 1;
  let lineStart = 0;
  let index = 0;

  while (index < source.length) {
    const character = source[index];

    if (source.startsWith('//', index)) {
      index = source.indexOf('\n', index);
      index = index < 0 ? source.length : index;
    } else if (source.startsWith('/*', index)) {
      const end = source.indexOf('*/', index + 2);
      const stop = end < 0 ? source.length : end + 2;

      for (let at = index; at < stop; ++at) {
        if (source[at] === '\n') {
          ++line;
          lineStart = at + 1;
        }
      }

      index = stop;
    } else if (character === 'R' && source[index + 1] === '"') {
      // A raw string is code here: changing it needs a rebuild.
      const open = source.indexOf('(', index);
      const closing = `)${source.slice(index + 2, open)}"`;
      const end = source.indexOf(closing, open) + closing.length;
      const text = source.slice(index, end);

      code += text;

      for (let at = index; at < end; ++at) {
        if (source[at] === '\n') {
          ++line;
          lineStart = at + 1;
        }
      }

      index = end;
    } else if (character === '"') {
      let end = index + 1;

      while (end < source.length && source[end] !== '"') {
        end += source[end] === '\\' ? 2 : 1;
      }

      const call = /([A-Za-z_][A-Za-z_0-9]*)\s*\(\s*$/.exec(code);
      const after = /^\s*([),])/.exec(source.slice(end + 1));
      const name = call?.[1];

      literals.push({
        raw: source.slice(index, end + 1),
        line,
        column: index - lineStart + 1,
        replaceable: replaceable.has(name) && after?.[1] === ')' && !code.endsWith('include '),
      });
      code += '""';
      index = end + 1;
    } else if (character === "'") {
      const end = source.indexOf("'", index + (source[index + 1] === '\\' ? 3 : 2));

      code += source.slice(index, end + 1);
      index = end + 1;
    } else {
      if (character === '\n') {
        ++line;
        lineStart = index + 1;
      }

      code += character;
      ++index;
    }
  }

  return { code, literals };
}

// What differs between what was compiled and the source now: literals to replace, or null when it takes a rebuild.
export function literalChanges(compiled, current) {
  if (compiled.code !== current.code || compiled.literals.length !== current.literals.length) {
    return null;
  }

  const changes = [];

  for (const [index, literal] of compiled.literals.entries()) {
    const now = current.literals[index];

    if (literal.raw !== now.raw) {
      if (!literal.replaceable || !now.replaceable) {
        return null;
      }

      // Where it was compiled: other literals changing on that line do not move it, for the program.
      changes.push({ line: literal.line, column: literal.column, value: JSON.parse(now.raw) });
    }
  }

  return changes;
}

function run(command, args, cwd) {
  return new Promise((done) => {
    let output = '';
    const child = spawn(command, args, { cwd, env: process.env });

    child.stdout.on('data', (data) => (output += data));
    child.stderr.on('data', (data) => (output += data));
    child.on('close', (code) => done({ code, output }));
  });
}

// A failed build, from its first error: what precedes is the build tool's own output, and the compiler's command line.
export function firstError(output) {
  const lines = output.split('\n');
  const first = lines.findIndex((line) => /\berror:/.test(line));

  return first < 0 ? output : lines.slice(first).join('\n');
}

export function quoll({ target, sources, buildDir = 'build' }) {
  const base = process.cwd();
  let config;

  const build = async (dir, options) => {
    if (!existsSync(join(dir, 'CMakeCache.txt'))) {
      const configured = await run('emcmake', ['cmake', '-S', root, '-B', dir, '-DQUOLL_BUILD_TESTS=OFF', ...options], base);

      if (configured.code !== 0) {
        return configured;
      }
    }

    return run('cmake', ['--build', dir, '--target', target], base);
  };

  const artifact = (dir, extension) =>
    join(dir, relative(root, base), `${target}${extension}`);

  return {
    name: 'quoll',

    configResolved(resolved) {
      config = resolved;
    },

    async configureServer(server) {
      const dir = resolve(base, buildDir);
      const watched = sources.map((source) => resolve(base, source));
      const compiled = new Map();

      const remember = () => {
        for (const file of watched) {
          compiled.set(file, scan(readFileSync(file, 'utf8')));
        }
      };

      const rebuild = async () => {
        const started = Date.now();
        const built = await build(dir, ['-DCMAKE_BUILD_TYPE=Debug', '-DQUOLL_UI_HOT=ON']);

        if (built.code !== 0) {
          server.config.logger.error(built.output);
          server.ws.send({ type: 'error', err: { message: firstError(built.output), stack: '' } });
          return false;
        }

        remember();
        server.config.logger.info(`quoll: built in ${Date.now() - started} ms`);
        return true;
      };

      server.middlewares.use((request, response, next) => {
        const extension = { [`/${target}.js`]: '.js', [`/${target}.wasm`]: '.wasm' }[request.url];

        if (!extension) {
          next();
          return;
        }

        response.setHeader('Content-Type', extension === '.wasm' ? 'application/wasm' : 'text/javascript');
        createReadStream(artifact(dir, extension)).pipe(response);
      });

      // One build at a time: a change during a build gets one more, which sees it.
      let building = false;
      let changedMeanwhile = false;

      const rebuildAndReload = async () => {
        if (building) {
          changedMeanwhile = true;
          return;
        }

        building = true;

        do {
          changedMeanwhile = false;

          if ((await rebuild()) && !changedMeanwhile) {
            server.ws.send({ type: 'full-reload' });
          }
        } while (changedMeanwhile);

        building = false;
      };

      server.watcher.add(watched);
      server.watcher.on('change', async (file) => {
        if (!watched.includes(file)) {
          return;
        }

        const changes = building ? null : literalChanges(compiled.get(file), scan(readFileSync(file, 'utf8')));

        if (changes) {
          for (const change of changes) {
            server.ws.send({ type: 'custom', event: 'quoll:literal', data: { file, ...change } });
          }

          return;
        }

        await rebuildAndReload();
      });

      if (!(await rebuild())) {
        throw new Error('quoll: the first build failed');
      }
    },

    async generateBundle() {
      const dir = resolve(base, `${buildDir}-release`);
      const built = await build(dir, ['-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_CXX_FLAGS_RELEASE=-Oz -DNDEBUG']);

      if (built.code !== 0) {
        this.error(built.output);
      }

      for (const extension of ['.js', '.wasm']) {
        this.emitFile({
          type: 'asset',
          fileName: `${target}${extension}`,
          source: readFileSync(artifact(dir, extension)),
        });
      }
    },
  };
}
