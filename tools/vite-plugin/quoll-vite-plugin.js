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


// The planned `quoll/vite` plugin, until it is a package of its own: builds a program of C++ with CMake, serves it,
// reloads it, and makes the registry of its JavaScript widgets (virtual:quoll-widgets), each module loaded on demand.
//
// Three levels, from the cheapest:
// 1. CSS: Vite and Tailwind's own plugin, which reads the C++ for class names.
// 2. A text or a class list changed in the source: sent to the running program, which keeps its state. No compilation.
// 3. Anything else: a rebuild, then the page reloads. A compilation error shows in Vite's overlay. A source made in a
//    folder of `sources`, or removed from it, is one of them, or no longer, and rebuilds the page as well.

import { spawn } from 'node:child_process';
import { createReadStream, existsSync, readdirSync, readFileSync, realpathSync, statSync } from 'node:fs';
import { dirname, extname, isAbsolute, join, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');

// What takes one literal that hot reload can replace: tw(), text() and the tags of quoll/ui/view/tags.hpp.
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

// The paths of the widgets a C++ source declares, `widget<Props>("./path.js", "tag")`, with braces, or named as a variable
// (`widget<Props> chart{"./path.js", "tag"}`), in the order they come: found in the code scan() leaves, comments gone and each string blanked, then read from the string at that rank.
export function widgetPaths(source) {
  const { code, literals } = scan(source);
  const paths = [];

  for (const match of code.matchAll(/\bwidget\s*<[^;{}()"]*>\s*(?:[A-Za-z_]\w*\s*)?[({]\s*""/g)) {
    // The strings before it, but for raw ones, which scan() keeps apart.
    const rank = [...code.slice(0, match.index + match[0].length - 2).matchAll(/(?<!R)""/g)].length;
    const path = decoded(literals[rank].raw);

    if (typeof path === 'string') {
      paths.push(path);
    }
  }

  return paths;
}

// The registry the page sets as globalThis.quollWidgets: each path a source declares, resolved from that source as an
// import would be, to a function importing its module, which a build makes a chunk loaded on demand. `files` are the
// sources, with their text. Throws for one path given to two modules.
export function widgetRegistry(files) {
  const modules = new Map();

  for (const [file, text] of files) {
    for (const path of widgetPaths(text)) {
      // As an import names it: relative to its source, or a package, or a URL, as it is.
      const module = path.startsWith('./') || path.startsWith('../') ? resolve(dirname(file), path) : path;
      const known = modules.get(path);

      if (known !== undefined && known !== module) {
        throw new Error(`quoll: the widget ${path} names ${known} and ${module}`);
      }

      modules.set(path, module);
    }
  }

  // A file by its absolute path, which Vite and the build both read, inside its root or not.
  const specifier = (module) => (isAbsolute(module) ? module.split(sep).join('/') : module);
  const entries = [...modules].map(
    ([path, module]) => `  ${JSON.stringify(path)}: () => import(${JSON.stringify(specifier(module))}),`,
  );

  return `export default {\n${entries.join('\n')}\n};\n`;
}

const registryId = 'virtual:quoll-widgets';
const resolvedRegistryId = `\0${registryId}`;

// The debug information of a development build, by `debug`: file and line in a trace by default, which halves what the
// backend writes; `full` for variables in the browser's debugger, `none` for the shortest rebuilds.
const debugFlags = { lines: '-gline-tables-only', full: '-g', none: '-g0' };

// The CMake options of a development build: hot reload, and the debug information `debug` names.
export function developmentOptions(debug = 'lines') {
  if (!Object.hasOwn(debugFlags, debug)) {
    throw new Error(`quoll: debug is one of ${Object.keys(debugFlags).join(', ')}, not ${debug}`);
  }

  return [
    '-DCMAKE_BUILD_TYPE=Debug',
    `-DCMAKE_C_FLAGS_DEBUG=${debugFlags[debug]}`,
    `-DCMAKE_CXX_FLAGS_DEBUG=${debugFlags[debug]}`,
    '-DQUOLL_UI_HOT=ON',
  ];
}

// Whether a path is in one of these directories, or one of them: compared as paths, not as globs, which a directory
// named with brackets or parentheses would break.
export function within(directories) {
  return (path) => directories.some((directory) => path === directory || path.startsWith(directory + sep));
}

// What a C++ source is named, which a directory of `sources` gives.
const cppExtensions = new Set(['.cpp', '.cc', '.cxx', '.hpp', '.hh', '.h', '.ipp']);

// What `sources` names, from `base`: its folders and its files. A path that is not there yet is a file when it has an
// extension, a folder otherwise.
export function sourcePaths(base, sources) {
  const paths = { folders: [], files: [] };

  for (const path of sources.map((source) => resolve(base, source))) {
    const folder = statSync(path, { throwIfNoEntry: false })?.isDirectory() ?? extname(path) === '';

    (folder ? paths.folders : paths.files).push(path);
  }

  return paths;
}

// Whether a file of a folder of the sources is one of them: a C++ file, not in `ignored`, as the build directories are,
// nor in a node_modules under that folder.
function inFolder(folder, path, ignored) {
  return (
    cppExtensions.has(extname(path)) && !ignored(path) && !relative(folder, path).split(sep).includes('node_modules')
  );
}

// Whether `path` is a source of the page: one of the files `paths` names, whatever its extension, or a C++ file of one of
// its folders, as `listSources` finds them.
export function isSource(path, paths, ignored = () => false) {
  return (
    paths.files.includes(path) || paths.folders.some((folder) => within([folder])(path) && inFolder(folder, path, ignored))
  );
}

// The sources there now, in order: the files named, and every C++ file of the folders, at any depth, links followed, once
// each, so that a page's sources are its folders rather than a list kept by hand. The walk does not go into what
// `ignored` holds, nor a node_modules, nor a folder it has walked. A folder not there, as one named and not made yet or
// one removed during the walk, holds none; one that cannot be read is told to `warn`.
export function listSources(paths, ignored = () => false, warn = () => {}) {
  const found = new Set(paths.files);
  const walked = new Set();

  const walk = (folder, directory) => {
    let entries = [];

    try {
      const real = realpathSync(directory);

      if (walked.has(real)) {
        return;
      }

      walked.add(real);
      entries = readdirSync(directory, { withFileTypes: true });
    } catch (error) {
      if (error.code !== 'ENOENT') {
        warn(`quoll: ${directory} cannot be read: ${error.message}`);
      }
    }

    for (const entry of entries) {
      const path = join(directory, entry.name);
      const target = entry.isSymbolicLink() ? statSync(path, { throwIfNoEntry: false }) : entry;

      if (target?.isDirectory() && entry.name !== 'node_modules' && !ignored(path)) {
        walk(folder, path);
      } else if (target?.isFile() && inFolder(folder, path, ignored)) {
        found.add(path);
      }
    }
  };

  for (const folder of paths.folders) {
    walk(folder, folder);
  }

  return [...found].sort();
}

export function quoll({ target, sources, buildDir = 'build', debug = 'lines' }) {
  // At once: a wrong option is told as Vite starts, not at the first build.
  const development = developmentOptions(debug);

  const base = process.cwd();
  // What the build writes, which Vite does not watch, nor a folder of the sources lists.
  const builds = within([resolve(base, buildDir), resolve(base, `${buildDir}-release`)]);
  const paths = sourcePaths(base, sources);
  // Where a folder that cannot be read is told, once each: Vite's logger once its server runs.
  const told = new Set();
  let logger = console;

  const warn = (message) => {
    if (!told.has(message)) {
      told.add(message);
      logger.warn(message);
    }
  };
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

    // Its build directories, which CMake fills with files that come and go, are not the page's: watching them is no use,
    // and a file gone between being listed and being watched stops Vite.
    config() {
      return { server: { watch: { ignored: builds } } };
    },

    resolveId(id) {
      return id === registryId ? resolvedRegistryId : null;
    },

    load(id) {
      if (id !== resolvedRegistryId) {
        return null;
      }

      // Not watched through the module graph, which would send a change of a source up to the page and reload it: the
      // server's watcher makes the registry again as a source changes.
      const files = listSources(paths, builds, warn);

      // A source gone for a moment, during a save, has no widget until it is back: the build says what is wrong.
      const readable = files.flatMap((file) => {
        try {
          return [[file, readFileSync(file, 'utf8')]];
        } catch {
          return [];
        }
      });

      return widgetRegistry(readable);
    },

    async configureServer(server) {
      const dir = resolve(base, buildDir);
      logger = server.config.logger;
      const source = (file) => isSource(file, paths, builds);
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

        // The sources as they are now, a file made in a folder, or removed, since the last build included. Before the
        // build: a save during it then differs from this, and is not taken for what was compiled.
        const files = listSources(paths, builds, warn);
        const readSources = files.map((file) => [file, read(file)]);
        const built = await build(dir, development);

        failed = built.code !== 0;

        if (failed) {
          server.config.logger.error(built.output);
          server.ws.send({ type: 'error', err: { message: firstError(built.output), stack: '' } });
          return false;
        }

        // What was compiled is what this build read: a file gone since leaves no entry.
        compiled.clear();
        sent.clear();

        for (const [file, scanned] of readSources) {
          compiled.set(file, scanned);
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

      // Once: what the build reads is what is under these, which the watcher tells of as it changes.
      server.watcher.add([...paths.folders, ...paths.files]);

      // The widgets' registry, made again as the page reloads.
      const invalidateRegistry = () => {
        const registry = server.moduleGraph.getModuleById(resolvedRegistryId);

        if (registry) {
          server.moduleGraph.invalidateModule(registry);
        }
      };

      // A source saved: its literals sent to the program when they are all that changed, a rebuild otherwise, as for one
      // never compiled.
      const changed = async (file) => {
        if (!source(file)) {
          return;
        }

        // Its widgets may have changed.
        invalidateRegistry();

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
      };

      // A source made is a change, as one never compiled: a rebuild with it, or, as a save writing a new file in place of
      // the old makes it again, its literals alone.
      server.watcher.on('add', changed);
      server.watcher.on('change', changed);

      // A source removed: the page rebuilds without it, unless it is back a moment later, as a save writing a new file in
      // place of the old makes it where the watcher tells that as a removal and an addition, which is the change.
      server.watcher.on('unlink', (file) => {
        if (!source(file)) {
          return;
        }

        setTimeout(() => {
          if (!existsSync(file)) {
            invalidateRegistry();
            rebuildAndReload();
          }
        }, 100);
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
