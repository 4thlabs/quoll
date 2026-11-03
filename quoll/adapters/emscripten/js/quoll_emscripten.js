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


// JavaScript side of quoll::emscripten, linked into the application with --js-library by the quoll::emscripten
// target. The C++ declarations are in quoll/emscripten/detail/js.hpp.

addToLibrary({
  // Fetches started and not released yet, by id: the abort controller, then the status and the parts of the response
  // (or the failure message) once settled.
  $quollFetches: {
    next: 1,
    entries: {},
  },

  // Resumes on the event loop, as a microtask: like the continuation of a JavaScript await.
  quoll_emscripten_post__deps: ['$callUserCallback'],
  quoll_emscripten_post: (resume, item) => {
    {{{ runtimeKeepalivePush() }}}
    queueMicrotask(() => {
      {{{ runtimeKeepalivePop() }}}
      callUserCallback(() => {{{ makeDynCall('vp', 'resume') }}}(item));
    });
  },

  // Starts fetch(url, {method, headers, body}). `headers` holds one "name:value\n" line per field. Calls
  // complete(operation, id, outcome) once settled, outcome being 0 (response), 1 (aborted) or 2 (failure).
  quoll_emscripten_fetch_start__deps: ['$quollFetches', '$UTF8ToString', '$callUserCallback'],
  quoll_emscripten_fetch_start: (url, method, headers, body, bodySize, complete, operation) => {
    const id = quollFetches.next++;
    const entry = { controller: new AbortController(), status: 0, parts: [] };
    const init = { method: UTF8ToString(method), headers: [], signal: entry.controller.signal };
    const encoder = new TextEncoder();

    for (const line of UTF8ToString(headers).split('\n')) {
      const colon = line.indexOf(':');

      if (colon > 0) {
        init.headers.push([line.slice(0, colon), line.slice(colon + 1)]);
      }
    }

    // Copied now: the request may change once the operation resumes.
    if (bodySize > 0) {
      init.body = HEAPU8.slice(body, body + bodySize);
    }

    const settle = async () => {
      try {
        const response = await fetch(UTF8ToString(url), init);
        const fields = [];

        response.headers.forEach((value, name) => fields.push(`${name}:${value}\n`));
        entry.parts = [
          encoder.encode(response.statusText),
          encoder.encode(fields.join('')),
          new Uint8Array(await response.arrayBuffer()),
        ];
        entry.status = response.status;
        return 0;
      } catch (error) {
        // Node's fetch tells why in the cause (ECONNREFUSED...).
        const cause = error.cause ? `: ${error.cause.message ?? error.cause}` : '';

        entry.parts = [encoder.encode(`${error.message ?? error}${cause}`), new Uint8Array(), new Uint8Array()];
        return error.name === 'AbortError' ? 1 : 2;
      }
    };

    quollFetches.entries[id] = entry;
    {{{ runtimeKeepalivePush() }}}
    settle().then((outcome) => {
      {{{ runtimeKeepalivePop() }}}
      callUserCallback(() => {{{ makeDynCall('vpii', 'complete') }}}(operation, id, outcome));
    });
    return id;
  },

  // Ignores a fetch already released.
  quoll_emscripten_fetch_abort__deps: ['$quollFetches'],
  quoll_emscripten_fetch_abort: (id) => quollFetches.entries[id]?.controller.abort(),

  quoll_emscripten_fetch_status__deps: ['$quollFetches'],
  quoll_emscripten_fetch_status: (id) => quollFetches.entries[id].status,

  // Part 0: status text, or failure message; 1: header fields, "name:value\n" each; 2: body.
  quoll_emscripten_fetch_size__deps: ['$quollFetches'],
  quoll_emscripten_fetch_size: (id, part) => quollFetches.entries[id].parts[part].length,

  quoll_emscripten_fetch_copy__deps: ['$quollFetches'],
  quoll_emscripten_fetch_copy: (id, part, out) => HEAPU8.set(quollFetches.entries[id].parts[part], out),

  quoll_emscripten_fetch_release__deps: ['$quollFetches'],
  quoll_emscripten_fetch_release: (id) => {
    delete quollFetches.entries[id];
  },
});
