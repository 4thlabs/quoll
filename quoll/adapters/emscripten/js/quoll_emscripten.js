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
  // (or the failure message) once settled, and the reader of a body read in pieces.
  $quollFetches: {
    next: 1,
    entries: {},
  },

  // The exit status of a Node process, once its loop has drained: none in a browser, which reads as 0.
  quoll_emscripten_exit_code: () =>
    typeof process === 'object' && process.versions && process.versions.node ? (process.exitCode ?? 0) : 0,
  quoll_emscripten_set_exit_code: (code) => {
    if (typeof process === 'object' && process.versions && process.versions.node) {
      process.exitCode = code;
    }
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
  // those nodes, their widgets by node, and what events call.
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

    quollUi.instances[instance] = {
      nodes: new Map([[0, root]]),
      listeners: new Map(),
      widgets: new Map(),
      dispatch: 0,
      owner: 0,
      fail: 0,
      failOwner: 0,
    };
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

  // A failure the app's on_error hears of, once the call that caused it has returned: a widget's module failing to load
  // or to mount.
  $quollUiReport__deps: ['$stringToNewUTF8', '$callUserCallback', 'free'],
  $quollUiReport: (ui, message) => {
    console.error(message);

    if (!ui.fail) {
      return;
    }

    callUserCallback(() => {
      const pointer = stringToNewUTF8(String(message));

      try {
        {{{ makeDynCall('vpp', 'ui.fail') }}}(ui.failOwner, pointer);
      } finally {
        _free(pointer);
      }
    });
  },

  // Whether `node` is `ancestor` or within it, in the page as in the tests' DOM.
  $quollUiWithin: (node, ancestor) => {
    for (let current = node; current; current = current.parentNode ?? current.parent) {
      if (current === ancestor) {
        return true;
      }
    }

    return false;
  },

  // Takes a widget out of its module: nothing it emits reaches the app afterwards. True when its unmount threw.
  $quollUiUnmountWidget: (ui, id) => {
    const widget = ui.widgets.get(id);

    if (!widget) {
      return false;
    }

    ui.widgets.delete(id);
    widget.gone = true;

    try {
      widget.handle?.unmount?.();
      return false;
    } catch (error) {
      console.error('quoll: a widget failed to unmount', error);
      return true;
    }
  },

  quoll_emscripten_ui_on_failure__deps: ['$quollUi'],
  quoll_emscripten_ui_on_failure: (instance, fail, owner) => {
    quollUi.instances[instance].fail = fail;
    quollUi.instances[instance].failOwner = owner;
  },

  // A widget: its module, loaded from the registry the page sets in globalThis.quollWidgets (the Vite plugin makes it),
  // mounted once loaded with the props it holds then, the last an update gave; a mount that returns a promise is
  // awaited. 1 when the registry has no such module.
  quoll_emscripten_ui_mount_widget__deps: [
    '$quollUi',
    '$quollUiReport',
    '$quollUiUnmountWidget',
    '$UTF8ToString',
    '$stringToNewUTF8',
    '$callUserCallback',
    'free',
  ],
  quoll_emscripten_ui_mount_widget: (instance, id, module, moduleSize, props, propsSize) => {
    const ui = quollUi.instances[instance];
    const path = UTF8ToString(module, moduleSize);
    const registry = globalThis.quollWidgets;
    // Its own keys only: a path such as "constructor" is not the registry's.
    const load = registry && Object.hasOwn(registry, path) ? registry[path] : undefined;

    if (typeof load !== 'function') {
      return 1;
    }

    // A node mounted again, by a renderer's user ahead of its view: the widget it held goes first.
    quollUiUnmountWidget(ui, id);

    const widget = { props: UTF8ToString(props, propsSize), handle: null, gone: false };

    // What the module emits is an event of its node, its value as JSON; nothing once it is unmounted, nor what JSON
    // cannot hold, which the console shows.
    const emit = (name, value) => {
      if (widget.gone) {
        return;
      }

      let eventName;
      let text;

      try {
        eventName = String(name);
        text = JSON.stringify(value ?? null);
      } catch (error) {
        console.error(`quoll: the widget ${path} emitted what JSON cannot hold`, error);
        return;
      }

      // A function, a symbol, or what toJSON() turns into nothing: JSON.stringify gives no string for it, and no throw.
      if (typeof text !== 'string') {
        console.error(`quoll: the widget ${path} emitted what JSON cannot hold`);
        return;
      }

      callUserCallback(() => {
        const namePointer = stringToNewUTF8(eventName);
        const valuePointer = stringToNewUTF8(text);

        try {
          {{{ makeDynCall('vpipp', 'ui.dispatch') }}}(ui.owner, id, namePointer, valuePointer);
        } finally {
          _free(namePointer);
          _free(valuePointer);
        }
      });
    };

    ui.widgets.set(id, widget);
    {{{ runtimeKeepalivePush() }}}

    let mountedWith;

    Promise.resolve()
      .then(load)
      .then((loaded) => {
        // Unmounted while it loaded: nothing to mount.
        if (widget.gone) {
          return null;
        }

        mountedWith = widget.props;
        return loaded.mount(ui.nodes.get(id), JSON.parse(mountedWith), emit);
      })
      .then((handle) => {
        if (widget.gone) {
          // Unmounted while its mount ran: what it made goes, and a failure of that is told as any unmount's.
          try {
            handle?.unmount?.();
          } catch (error) {
            quollUiReport(ui, `quoll: the widget ${path} failed to unmount: ${error}`);
          }

          return;
        }

        widget.handle = handle ?? {};

        // Props given while its mount ran.
        if (widget.props !== mountedWith) {
          try {
            widget.handle.update?.(JSON.parse(widget.props));
          } catch (error) {
            quollUiReport(ui, `quoll: the widget ${path} failed to update: ${error}`);
          }
        }
      })
      .catch((error) => {
        if (!widget.gone) {
          quollUiReport(ui, `quoll: the widget ${path} failed to load or to mount: ${error}`);
        }
      })
      .finally(() => {
        {{{ runtimeKeepalivePop() }}}
      });

    return 0;
  },

  // Kept for its mount while it loads. 1 when its update threw, which the console shows.
  quoll_emscripten_ui_update_widget__deps: ['$quollUi', '$UTF8ToString'],
  quoll_emscripten_ui_update_widget: (instance, id, props, size) => {
    const widget = quollUi.instances[instance].widgets.get(id);

    if (!widget) {
      return 0;
    }

    widget.props = UTF8ToString(props, size);

    if (!widget.handle?.update) {
      return 0;
    }

    try {
      widget.handle.update(JSON.parse(widget.props));
      return 0;
    } catch (error) {
      console.error('quoll: a widget failed to update', error);
      return 1;
    }
  },

  // 1 when its unmount threw, which the console shows: it is gone all the same.
  quoll_emscripten_ui_unmount_widget__deps: ['$quollUi', '$quollUiUnmountWidget'],
  quoll_emscripten_ui_unmount_widget: (instance, id) => (quollUiUnmountWidget(quollUi.instances[instance], id) ? 1 : 0),

  quoll_emscripten_ui_close__deps: ['$quollUi', '$quollUiUnmountWidget'],
  quoll_emscripten_ui_close: (instance) => {
    const ui = quollUi.instances[instance];

    // Its widgets first, which their nodes hold.
    for (const id of [...ui.widgets.keys()]) {
      quollUiUnmountWidget(ui, id);
    }

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

  quoll_emscripten_ui_remove__deps: ['$quollUi', '$quollUiUnmountWidget', '$quollUiWithin'],
  quoll_emscripten_ui_remove: (instance, id) => {
    const ui = quollUi.instances[instance];

    // Unmounted already, but by a renderer's user that did not: the modules of the node, and of those within it, let go
    // of them before they go.
    const removed = ui.nodes.get(id);

    for (const widget of [...ui.widgets.keys()]) {
      if (quollUiWithin(ui.nodes.get(widget), removed)) {
        quollUiUnmountWidget(ui, widget);
      }
    }

    removed.remove();
    ui.nodes.delete(id);

    for (const key of ui.listeners.keys()) {
      if (key.startsWith(`${id}:`)) {
        ui.listeners.delete(key);
      }
    }
  },

  // What samples/ui_notes/src/app.js and the like call when the development server says a literal has changed.
  quoll_emscripten_ui_hot_install__deps: ['$stringToNewUTF8', '$callUserCallback', 'free'],
  quoll_emscripten_ui_hot_install: (replace) => {
    globalThis.quollHotReplace = (file, line, column, value) => {
      callUserCallback(() => {
        const filePointer = stringToNewUTF8(file);
        const valuePointer = stringToNewUTF8(value);

        {{{ makeDynCall('vpiip', 'replace') }}}(filePointer, line, column, valuePointer);
        _free(filePointer);
        _free(valuePointer);
      });
    };

    // A page reloading asks again for what it was sent, now that the program can take it.
    globalThis.quollHotReady?.();
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

  // Keeps why a fetch failed as its part 0, and says how: 1 (aborted by quoll_emscripten_fetch_abort) or 2 (failure).
  $quollFetchFailed: (entry, error) => {
    // Node's fetch tells why in the cause (ECONNREFUSED...).
    const cause = error.cause ? `: ${error.cause.message ?? error.cause}` : '';

    entry.parts = [new TextEncoder().encode(`${error.message ?? error}${cause}`), new Uint8Array(), new Uint8Array()];
    // Only our own abort is a cancellation: the browser aborts too (page unloaded), as a failure.
    return entry.controller.signal.aborted ? 1 : 2;
  },

  // Starts fetch(url, {method, headers, body}). `headers` holds one "name:value\n" line per field. Calls
  // complete(operation, id, outcome) once settled, outcome being 0 (response), 1 (aborted by
  // quoll_emscripten_fetch_abort) or 2 (failure). It settles with the head of the response, whose body
  // quoll_emscripten_fetch_read then reads.
  quoll_emscripten_fetch_start__deps: [
    '$quollFetches',
    '$quollFetchFailed',
    '$quollBytesToString',
    '$quollStringToBytes',
    '$UTF8ToString',
    '$callUserCallback',
  ],
  quoll_emscripten_fetch_start: (
    url, method, methodSize, headers, headersSize, body, bodySize, complete, operation) => {
    const id = quollFetches.next++;
    const entry = { controller: new AbortController(), status: 0, parts: [], reader: null };
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

    // complete() runs in the continuation of what it awaits, with no hop of its own: a stop in between would abort what
    // has just settled.
    const settle = async () => {
      let outcome = 0;

      try {
        const response = await fetch(UTF8ToString(url), init);
        const fields = [];

        response.headers.forEach((value, name) => fields.push(`${name}:${value}\n`));

        // A response with no body at all (HEAD, 204) has no reader either.
        entry.reader = response.body?.getReader() ?? null;

        entry.parts = [
          quollStringToBytes(response.statusText),
          quollStringToBytes(fields.join('')),
          new Uint8Array(),
        ];
        entry.status = response.status;
      } catch (error) {
        outcome = quollFetchFailed(entry, error);
      }

      {{{ runtimeKeepalivePop() }}}
      callUserCallback(() => {{{ makeDynCall('vpii', 'complete') }}}(operation, id, outcome));
    };

    quollFetches.entries[id] = entry;
    {{{ runtimeKeepalivePush() }}}
    settle();
    return id;
  },

  // Whether a fetch has a body to read.
  quoll_emscripten_fetch_has_body__deps: ['$quollFetches'],
  quoll_emscripten_fetch_has_body: (id) => (quollFetches.entries[id].reader ? 1 : 0),

  // Reads the next piece of a body, one JavaScript read at a time, and holds only that one, as part 2. Calls
  // complete(operation, outcome), outcome being 0 (a piece), 1 (aborted by quoll_emscripten_fetch_abort), 2 (failure) or
  // 3 (the end of the body). One at a time: the entry stays until it has completed.
  quoll_emscripten_fetch_read__deps: ['$quollFetches', '$quollFetchFailed', '$callUserCallback'],
  quoll_emscripten_fetch_read: (id, complete, operation) => {
    const entry = quollFetches.entries[id];

    // As in quoll_emscripten_fetch_start: no hop before complete().
    const settle = async () => {
      let outcome = 0;

      try {
        const { done, value } = await entry.reader.read();

        if (done) {
          outcome = 3;
        } else {
          entry.parts[2] = value;
        }
      } catch (error) {
        outcome = quollFetchFailed(entry, error);
      }

      {{{ runtimeKeepalivePop() }}}
      callUserCallback(() => {{{ makeDynCall('vpi', 'complete') }}}(operation, outcome));
    };

    {{{ runtimeKeepalivePush() }}}
    settle();
  },

  // Ignores a fetch already released.
  quoll_emscripten_fetch_abort__deps: ['$quollFetches'],
  quoll_emscripten_fetch_abort: (id) => quollFetches.entries[id]?.controller.abort(),

  quoll_emscripten_fetch_status__deps: ['$quollFetches'],
  quoll_emscripten_fetch_status: (id) => quollFetches.entries[id].status,

  // Part 0: status text (bytes), or failure message (UTF-8); 1: header fields, "name:value\n" each (bytes); 2: the whole
  // body, or the piece a read has just given.
  quoll_emscripten_fetch_size__deps: ['$quollFetches'],
  quoll_emscripten_fetch_size: (id, part) => quollFetches.entries[id].parts[part].length,

  quoll_emscripten_fetch_copy__deps: ['$quollFetches'],
  // Handed over: JavaScript does not hold a piece as well for as long as the caller works on it.
  quoll_emscripten_fetch_copy: (id, part, out) => {
    const { parts } = quollFetches.entries[id];

    HEAPU8.set(parts[part], out);
    parts[part] = new Uint8Array();
  },

  quoll_emscripten_fetch_release__deps: ['$quollFetches'],
  quoll_emscripten_fetch_release: (id) => {
    delete quollFetches.entries[id];
  },
});
