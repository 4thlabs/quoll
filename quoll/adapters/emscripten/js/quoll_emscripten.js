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

  // The interfaces of the page, by instance: their nodes by id (0 is the element they are mounted in), the listeners of
  // those nodes, and what events call.
  $quollUi: {
    next: 1,
    instances: {},
  },

  quoll_emscripten_ui_create__deps: ['$quollUi', '$UTF8ToString'],
  quoll_emscripten_ui_create: (selector) => {
    const root = document.querySelector(UTF8ToString(selector));

    if (!root) {
      return 0;
    }

    const instance = quollUi.next++;

    quollUi.instances[instance] = { nodes: new Map([[0, root]]), listeners: new Map(), dispatch: 0, owner: 0 };
    return instance;
  },

  quoll_emscripten_ui_on_event__deps: ['$quollUi'],
  quoll_emscripten_ui_on_event: (instance, dispatch, owner) => {
    quollUi.instances[instance].dispatch = dispatch;
    quollUi.instances[instance].owner = owner;
  },

  quoll_emscripten_ui_create_element__deps: ['$quollUi', '$UTF8ToString'],
  quoll_emscripten_ui_create_element: (instance, id, tag, size) => {
    quollUi.instances[instance].nodes.set(id, document.createElement(UTF8ToString(tag, size)));
  },

  quoll_emscripten_ui_create_text__deps: ['$quollUi', '$UTF8ToString'],
  quoll_emscripten_ui_create_text: (instance, id, content, size) => {
    quollUi.instances[instance].nodes.set(id, document.createTextNode(UTF8ToString(content, size)));
  },

  quoll_emscripten_ui_set_text__deps: ['$quollUi', '$UTF8ToString'],
  quoll_emscripten_ui_set_text: (instance, id, content, size) => {
    quollUi.instances[instance].nodes.get(id).data = UTF8ToString(content, size);
  },

  quoll_emscripten_ui_set_class__deps: ['$quollUi', '$UTF8ToString'],
  quoll_emscripten_ui_set_class: (instance, id, classes, size) => {
    quollUi.instances[instance].nodes.get(id).className = UTF8ToString(classes, size);
  },

  quoll_emscripten_ui_set_attribute__deps: ['$quollUi', '$UTF8ToString'],
  quoll_emscripten_ui_set_attribute: (instance, id, name, nameSize, value, valueSize) => {
    quollUi.instances[instance].nodes.get(id).setAttribute(UTF8ToString(name, nameSize), UTF8ToString(value, valueSize));
  },

  quoll_emscripten_ui_remove_attribute__deps: ['$quollUi', '$UTF8ToString'],
  quoll_emscripten_ui_remove_attribute: (instance, id, name, size) => {
    quollUi.instances[instance].nodes.get(id).removeAttribute(UTF8ToString(name, size));
  },

  // Left alone when it holds the value already: writing it would move the cursor of someone typing.
  quoll_emscripten_ui_set_value__deps: ['$quollUi', '$UTF8ToString'],
  quoll_emscripten_ui_set_value: (instance, id, value, size) => {
    const node = quollUi.instances[instance].nodes.get(id);
    const text = UTF8ToString(value, size);

    if (node.value !== text) {
      node.value = text;
    }
  },

  quoll_emscripten_ui_listen__deps: ['$quollUi', '$UTF8ToString', '$stringToNewUTF8', '$callUserCallback', 'free'],
  quoll_emscripten_ui_listen: (instance, id, name, size) => {
    const ui = quollUi.instances[instance];
    const eventName = UTF8ToString(name, size);

    const listener = (event) => {
      // A form does not leave the page: an interface handles its submission.
      if (eventName === 'submit') {
        event.preventDefault();
      }

      callUserCallback(() => {
        const namePointer = stringToNewUTF8(eventName);
        const valuePointer = stringToNewUTF8(String(event.target?.value ?? ''));

        try {
          {{{ makeDynCall('vpipp', 'ui.dispatch') }}}(ui.owner, id, namePointer, valuePointer);
        } finally {
          _free(namePointer);
          _free(valuePointer);
        }
      });
    };

    ui.nodes.get(id).addEventListener(eventName, listener);
    ui.listeners.set(`${id}:${eventName}`, listener);
  },

  quoll_emscripten_ui_unlisten__deps: ['$quollUi', '$UTF8ToString'],
  quoll_emscripten_ui_unlisten: (instance, id, name, size) => {
    const ui = quollUi.instances[instance];
    const eventName = UTF8ToString(name, size);

    ui.nodes.get(id).removeEventListener(eventName, ui.listeners.get(`${id}:${eventName}`));
    ui.listeners.delete(`${id}:${eventName}`);
  },

  quoll_emscripten_ui_insert__deps: ['$quollUi'],
  quoll_emscripten_ui_insert: (instance, parent, child, before) => {
    const { nodes } = quollUi.instances[instance];

    nodes.get(parent).insertBefore(nodes.get(child), before === 0 ? null : nodes.get(before));
  },

  quoll_emscripten_ui_close__deps: ['$quollUi'],
  quoll_emscripten_ui_close: (instance) => {
    const ui = quollUi.instances[instance];

    for (const [key, listener] of ui.listeners) {
      const separator = key.indexOf(':');

      ui.nodes.get(Number(key.slice(0, separator))).removeEventListener(key.slice(separator + 1), listener);
    }

    for (const [id, node] of ui.nodes) {
      // 0 is the page's own element, which it was given to show in.
      if (id !== 0) {
        node.remove();
      }
    }

    delete quollUi.instances[instance];
  },

  quoll_emscripten_ui_remove__deps: ['$quollUi'],
  quoll_emscripten_ui_remove: (instance, id) => {
    const ui = quollUi.instances[instance];

    ui.nodes.get(id).remove();
    ui.nodes.delete(id);

    for (const key of ui.listeners.keys()) {
      if (key.startsWith(`${id}:`)) {
        ui.listeners.delete(key);
      }
    }
  },

  // Timers started and not settled yet, by id: the setTimeout handle and how to settle.
  $quollTimers: {
    next: 1,
    entries: {},
  },

  // Calls complete(operation, elapsed) once the time has elapsed, or once cancelled. Rounded up, as a timer never ends
  // early; setTimeout fires at once past its limit, a 32-bit count of milliseconds.
  quoll_emscripten_timer_start__deps: ['$quollTimers', '$callUserCallback'],
  quoll_emscripten_timer_start: (milliseconds, complete, operation) => {
    const id = quollTimers.next++;
    const settle = (elapsed) => {
      delete quollTimers.entries[id];
      {{{ runtimeKeepalivePop() }}}
      callUserCallback(() => {{{ makeDynCall('vpi', 'complete') }}}(operation, elapsed));
    };

    {{{ runtimeKeepalivePush() }}}
    quollTimers.entries[id] = {
      handle: setTimeout(() => settle(1), Math.min(Math.ceil(milliseconds), 2147483647)),
      settle,
    };
    return id;
  },

  // Ignores a timer already settled. The completion comes later, from the event loop: the stop request that cancels may
  // be made by the coroutine that would resume.
  quoll_emscripten_timer_cancel__deps: ['$quollTimers'],
  quoll_emscripten_timer_cancel: (id) => {
    const entry = quollTimers.entries[id];

    if (entry) {
      delete quollTimers.entries[id];
      clearTimeout(entry.handle);
      queueMicrotask(() => entry.settle(0));
    }
  },

  // HTTP methods, field values and status texts are byte strings: one byte per character, both ways.
  $quollBytesToString: (pointer, size) => String.fromCharCode(...HEAPU8.subarray(pointer, pointer + size)),
  $quollStringToBytes: (text) => Uint8Array.from(text, (character) => character.charCodeAt(0)),

  // Starts fetch(url, {method, headers, body}). `headers` holds one "name:value\n" line per field. Calls
  // complete(operation, id, outcome) once settled, outcome being 0 (response), 1 (aborted by
  // quoll_emscripten_fetch_abort) or 2 (failure).
  quoll_emscripten_fetch_start__deps: [
    '$quollFetches',
    '$quollBytesToString',
    '$quollStringToBytes',
    '$UTF8ToString',
    '$callUserCallback',
  ],
  quoll_emscripten_fetch_start: (
    url, method, methodSize, headers, headersSize, body, bodySize, complete, operation) => {
    const id = quollFetches.next++;
    const entry = { controller: new AbortController(), status: 0, parts: [] };
    const init = {
      method: quollBytesToString(method, methodSize),
      headers: [],
      signal: entry.controller.signal,
    };

    for (const line of quollBytesToString(headers, headersSize).split('\n')) {
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
          quollStringToBytes(response.statusText),
          quollStringToBytes(fields.join('')),
          new Uint8Array(await response.arrayBuffer()),
        ];
        entry.status = response.status;
        return 0;
      } catch (error) {
        // Node's fetch tells why in the cause (ECONNREFUSED...).
        const cause = error.cause ? `: ${error.cause.message ?? error.cause}` : '';

        const message = new TextEncoder().encode(`${error.message ?? error}${cause}`);

        entry.parts = [message, new Uint8Array(), new Uint8Array()];
        // Only our own abort is a cancellation: the browser aborts too (page unloaded), as a failure.
        return entry.controller.signal.aborted ? 1 : 2;
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

  // Part 0: status text (bytes), or failure message (UTF-8); 1: header fields, "name:value\n" each (bytes); 2: body.
  quoll_emscripten_fetch_size__deps: ['$quollFetches'],
  quoll_emscripten_fetch_size: (id, part) => quollFetches.entries[id].parts[part].length,

  quoll_emscripten_fetch_copy__deps: ['$quollFetches'],
  quoll_emscripten_fetch_copy: (id, part, out) => HEAPU8.set(quollFetches.entries[id].parts[part], out),

  quoll_emscripten_fetch_release__deps: ['$quollFetches'],
  quoll_emscripten_fetch_release: (id) => {
    delete quollFetches.entries[id];
  },
});
