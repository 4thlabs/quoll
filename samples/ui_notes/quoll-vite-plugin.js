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
import { createReadStream, readFileSync } from 'node:fs';
import { dirname, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');

// What takes one literal that hot reload can replace: tw(), text() and the tags of quoll/ui/tags.hpp.
const replaceable = new Set([
  'tw', 'text', 'a', 'article', 'button', 'div', 'footer', 'form', 'h1', 'h2', 'h3', 'header', 'label', 'li', 'main_',
  'nav', 'ol', 'p', 'section', 'span', 'ul',
]);

// Only a call to Quoll's function: not `other.text("x")`, `other::text("x")` or the declaration `std::string text("x")`.
function callsQuoll(before) {
  if (/(\.|->)\s*$/.test(before)) {
    return false;
  }

  const qualifier = /([A-Za-z_][A-Za-z_0-9]*)\s*::\s*$/.exec(before);

  if (qualifier) {
    return qualifier[1] === 'ui';
  }

  // After a pointer, a reference or a template, it is a declaration, like after a word. A wrong guess only takes a rebuild.
  if (/[*&>]\s*$/.test(before)) {
    return false;
  }

  const word = /([A-Za-z_][A-Za-z_0-9]*)\s+$/.exec(before);

  return !word || ['return', 'co_return', 'co_yield', 'co_await', 'case', 'else'].includes(word[1]);
}

// Spaces do not matter, as moving code around compiles to the same program. The end of a directive's line does.
function withoutSpaces(code) {
  return code
    .split('\n')
    .map((line) => line.replace(/\s+/g, ' ').trim())
    .filter((line) => line !== '')
    .map((line) => (line.startsWith('#') ? `\n${line}\n` : line))
    .join(' ');
}

// What a literal holds, or null for what JSON does not read the way C++ does, which then takes a rebuild.
function decoded(raw) {
  try {
    return JSON.parse(raw);
  } catch {
    return null;
  }
}

// Splits C++ into its code, with comments gone and strings blanked, its raw strings, and its string literals: where they
// are, and whether hot reload can replace them. Two sources with the same code and raw strings differ only in comments,
// spaces and literals.
export function scan(source) {
  let code = '';
  const raws = [];
  const literals = [];
  const lineStarts = [0];

  for (let at = 0; at < source.length; ++at) {
    if (source[at] === '\n') {
      lineStarts.push(at + 1);
    }
  }

  // Asked for growing offsets. The column is in bytes, as the compiler counts: an accent earlier on the line counts for two.
  let currentLine = 0;

  const locate = (offset) => {
    while (currentLine + 1 < lineStarts.length && lineStarts[currentLine + 1] <= offset) {
      ++currentLine;
    }

    return { line: currentLine + 1, column: Buffer.byteLength(source.slice(lineStarts[currentLine], offset)) + 1 };
  };

  let index = 0;

  while (index < source.length) {
    const character = source[index];

    if (source.startsWith('//', index)) {
      index = source.indexOf('\n', index);
      index = index < 0 ? source.length : index;
    } else if (source.startsWith('/*', index)) {
      const end = source.indexOf('*/', index + 2);

      index = end < 0 ? source.length : end + 2;
    } else if (character === 'R' && source[index + 1] === '"') {
      // A raw string is code here: changing it needs a rebuild.
      const open = source.indexOf('(', index);
      const closing = `)${source.slice(index + 2, open)}"`;
      const found = source.indexOf(closing, open);
      const end = found < 0 ? source.length : found + closing.length;

      raws.push(source.slice(index, end));
      code += 'R""';
      index = end;
    } else if (character === '"') {
      let end = index + 1;

      while (end < source.length && source[end] !== '"') {
        end += source[end] === '\\' ? 2 : 1;
      }

      // What precedes a literal is enough: not the whole file, for each literal.
      const tail = code.slice(-256);
      const call = /([A-Za-z_][A-Za-z_0-9]*)\s*\(\s*$/.exec(tail);
      const after = /^\s*([),])/.exec(source.slice(end + 1));
      const name = call?.[1];
      const isCall = call !== null && callsQuoll(tail.slice(0, call.index));

      literals.push({
        raw: source.slice(index, end + 1),
        ...locate(index),
        replaceable: isCall && replaceable.has(name) && after?.[1] === ')',
      });
      code += '""';
      index = end + 1;
    } else if (character === "'" && /(^|[^A-Za-z_0-9])[0-9][0-9a-fA-F']*$/.test(code.slice(-64)) && /[0-9a-fA-F]/.test(source[index + 1] ?? '')) {
      // A digit separator: 1'000'000.
      code += character;
      ++index;
    } else if (character === "'") {
      const end = source.indexOf("'", index + (source[index + 1] === '\\' ? 3 : 2));
      const stop = end < 0 ? source.length : end + 1;

      code += source.slice(index, stop);
      index = stop;
    } else {
      code += character;
      ++index;
    }
  }

  return { code: withoutSpaces(code), raws, literals };
}

// What to send for the source now, given what was compiled and what the page was already sent: literals to replace, or
// null when it takes a rebuild.
export function literalChanges(compiled, current, sent = new Map()) {
  if (
    !compiled ||
    !current ||
    compiled.code !== current.code ||
    compiled.raws.join('\0') !== current.raws.join('\0') ||
    compiled.literals.length !== current.literals.length
  ) {
    return null;
  }

  const changes = [];

  for (const [index, literal] of compiled.literals.entries()) {
    const now = current.literals[index];

    if (literal.raw !== now.raw && (!literal.replaceable || !now.replaceable)) {
      return null;
    }

    // Where it was compiled: other literals changing on that line do not move it, for the program. Against what the page
    // shows, so that going back to the compiled text is sent too.
    const place = `${literal.line}:${literal.column}`;

    if ((sent.get(place)?.raw ?? literal.raw) !== now.raw) {
      const value = decoded(now.raw);

      if (value === null) {
        return null;
      }

      changes.push({ line: literal.line, column: literal.column, raw: now.raw, value });
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
    // A tool that is not there reaches the same overlay as a failed build.
    child.on('error', (error) => done({ code: -1, output: `${command}: ${error.message}` }));
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
  const configured = new Set();

  const build = async (dir, options) => {
    // Once per start: a directory left by a failed configuration, or configured with other options, gets it right.
    if (!configured.has(dir)) {
      const result = await run('emcmake', ['cmake', '-S', root, '-B', dir, '-DQUOLL_BUILD_TESTS=OFF', ...options], base);

      if (result.code !== 0) {
        return result;
      }

      configured.add(dir);
    }

    return run('cmake', ['--build', dir, '--target', target], base);
  };

  const artifact = (dir, extension) =>
    join(dir, relative(root, base), `${target}${extension}`);

  return {
    name: 'quoll',

    async configureServer(server) {
      const dir = resolve(base, buildDir);
      const watched = sources.map((source) => resolve(base, source));
      const compiled = new Map();
      // What each file's page was sent since it was compiled, by place: a page reloading is sent it again.
      const sent = new Map();
      let failed = false;

      // A file gone for a moment, during a save or a change of branch, takes a rebuild, which says what is wrong.
      const read = (file) => {
        try {
          return scan(readFileSync(file, 'utf8'));
        } catch {
          return null;
        }
      };

      const rebuild = async () => {
        const started = Date.now();
        // Before the build: a save during it then differs from this, and is not taken for what was compiled.
        const sources = watched.map((file) => [file, read(file)]);
        const built = await build(dir, ['-DCMAKE_BUILD_TYPE=Debug', '-DQUOLL_UI_HOT=ON']);

        failed = built.code !== 0;

        if (failed) {
          server.config.logger.error(built.output);
          server.ws.send({ type: 'error', err: { message: firstError(built.output), stack: '' } });
          return false;
        }

        for (const [file, source] of sources) {
          compiled.set(file, source);
          sent.set(file, new Map());
        }

        server.config.logger.info(`quoll: built in ${Date.now() - started} ms`);
        return true;
      };

      server.middlewares.use((request, response, next) => {
        const path = request.url.split('?')[0];
        const extension = { [`/${target}.js`]: '.js', [`/${target}.wasm`]: '.wasm' }[path];

        if (!extension) {
          next();
          return;
        }

        response.setHeader('Content-Type', extension === '.wasm' ? 'application/wasm' : 'text/javascript');
        // The linker replaces these during a rebuild: the page asks again once it reloads.
        createReadStream(artifact(dir, extension))
          .on('error', () => {
            response.statusCode = 404;
            response.end();
          })
          .pipe(response);
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

        // After a failed build, whatever the source says now: the overlay goes when the page reloads.
        const changes = building || failed ? null : literalChanges(compiled.get(file), read(file), sent.get(file));

        if (changes) {
          for (const change of changes) {
            sent.get(file).set(`${change.line}:${change.column}`, change);
            server.ws.send({ type: 'custom', event: 'quoll:literal', data: { file, ...change } });
          }

          return;
        }

        await rebuildAndReload();
      });

      // A program starting again holds what was compiled, not what its page was sent.
      server.ws.on('quoll:ready', (_, client) => {
        for (const [file, changes] of sent) {
          for (const change of changes.values()) {
            client.send('quoll:literal', { file, ...change });
          }
        }
      });

      // In the same queue: a save during this one waits for it, even if it fails.
      await rebuildAndReload();

      if (failed) {
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
