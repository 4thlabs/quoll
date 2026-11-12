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


// The little of the DOM that quoll::emscripten::dom_renderer uses, for the tests running under Node, and functions to look
// at it and to send events to it. Runs what it can of the real code: only the browser's own objects are stood in for.

addToLibrary({
  $quollFakeDom: {
    root: null,
    Text: class {
      constructor(data) {
        this.data = data;
        this.parent = null;
      }

      remove() {
        this.parent?.children.splice(this.parent.children.indexOf(this), 1);
        this.parent = null;
      }

      html() {
        return this.data;
      }

      text() {
        return this.data;
      }
    },
    Element: class {
      constructor(tag) {
        this.tag = tag;
        this.className = '';
        this.value = undefined;
        this.checked = false;
        this.attributes = new Map();
        this.listeners = new Map();
        this.children = [];
        this.parent = null;
      }

      insertBefore(child, before) {
        child.remove();

        const place = before === null ? this.children.length : this.children.indexOf(before);

        this.children.splice(place, 0, child);
        child.parent = this;
      }

      remove() {
        this.parent?.children.splice(this.parent.children.indexOf(this), 1);
        this.parent = null;
      }

      get tagName() {
        return this.tag.toUpperCase();
      }

      // As a browser reflects the attribute.
      get type() {
        return this.attributes.get('type') ?? '';
      }

      setAttribute(name, value) {
        this.attributes.set(name, value);
      }

      removeAttribute(name) {
        this.attributes.delete(name);
      }

      hasAttribute(name) {
        return this.attributes.has(name);
      }

      getAttribute(name) {
        return this.attributes.has(name) ? this.attributes.get(name) : null;
      }

      // The listeners of a name, in the order added, as a form's own on submit and the app's: called as one.
      addEventListener(name, listener) {
        this.keepListeners(name, [...(this.listeners.get(name)?.parts ?? []), listener]);
      }

      removeEventListener(name, listener) {
        this.keepListeners(name, (this.listeners.get(name)?.parts ?? []).filter((part) => part !== listener));
      }

      keepListeners(name, parts) {
        if (parts.length === 0) {
          this.listeners.delete(name);
          return;
        }

        const all = (event) => parts.forEach((part) => part(event));

        all.parts = parts;
        this.listeners.set(name, all);
      }

      text() {
        return this.children.map((child) => child.text()).join('');
      }

      // In document order: the first element holding exactly this text, deepest first.
      find(text) {
        for (const child of this.children) {
          const found = child.find?.(text);

          if (found) {
            return found;
          }
        }

        return this.text() === text ? this : null;
      }

      findTag(tag) {
        for (const child of this.children) {
          if (child.tag === tag) {
            return child;
          }

          const found = child.findTag?.(tag);

          if (found) {
            return found;
          }
        }

        return null;
      }

      html() {
        const classes = this.className ? ` class="${this.className}"` : '';
        const value = this.value === undefined ? '' : ` value="${this.value}"`;
        const attributes = [...this.attributes].map(([name, content]) => ` ${name}="${content}"`).join('');

        return `<${this.tag}${classes}${attributes}${value}>${this.children.map((child) => child.html()).join('')}</${this.tag}>`;
      }
    },
  },

  // A new page, with one element: `#app`.
  quoll_test_dom_reset__deps: ['$quollFakeDom'],
  quoll_test_dom_reset: () => {
    quollFakeDom.root = new quollFakeDom.Element('root');
    globalThis.document = {
      querySelector: (selector) => (selector === '#app' ? quollFakeDom.root : null),
      createElement: (tag) => new quollFakeDom.Element(tag),
      createTextNode: (data) => new quollFakeDom.Text(data),
    };
  },

  // What the page shows: written into `out`, of `size` bytes.
  quoll_test_dom_html__deps: ['$quollFakeDom', '$stringToUTF8'],
  quoll_test_dom_html: (out, size) => {
    stringToUTF8(quollFakeDom.root.children.map((child) => child.html()).join(''), out, size);
  },

  // Clicks the element holding this text.
  quoll_test_dom_click__deps: ['$quollFakeDom', '$UTF8ToString'],
  quoll_test_dom_click: (text) => {
    const target = quollFakeDom.root.find(UTF8ToString(text));

    target.listeners.get('click')({ target });
  },

  // Clicks the element holding this text with the left button, a key held if `held`, as a browser does: whether the
  // page kept the browser from following it.
  quoll_test_dom_click_link__deps: ['$quollFakeDom', '$UTF8ToString'],
  quoll_test_dom_click_link: (text, held) => {
    const target = quollFakeDom.root.find(UTF8ToString(text));
    const event = {
      target,
      currentTarget: target,
      button: 0,
      ctrlKey: held !== 0,
      metaKey: false,
      shiftKey: false,
      altKey: false,
      defaultPrevented: false,
      preventDefault() {
        this.defaultPrevented = true;
      },
    };

    target.listeners.get('click')(event);
    return event.defaultPrevented ? 1 : 0;
  },

  // A location and a history as a browser's, starting at `start`, its popstate listeners on globalThis.
  quoll_test_history_install__deps: ['$UTF8ToString'],
  quoll_test_history_install: (start) => {
    const entries = [UTF8ToString(start)];
    let at = 0;
    const popped = new Set();
    const place = () => {
      const href = entries[at];
      const question = href.indexOf('?');

      globalThis.location = {
        pathname: question < 0 ? href : href.slice(0, question),
        search: question < 0 ? '' : href.slice(question),
      };
    };

    place();
    globalThis.history = {
      pushState(_state, _title, href) {
        entries.splice(at + 1);
        entries.push(href);
        at = entries.length - 1;
        place();
      },
      replaceState(_state, _title, href) {
        entries[at] = href;
        place();
      },
      back() {
        if (at > 0) {
          at -= 1;
          place();
          popped.forEach((listener) => listener());
        }
      },
    };
    globalThis.addEventListener = (name, listener) => name === 'popstate' && popped.add(listener);
    globalThis.removeEventListener = (name, listener) => name === 'popstate' && popped.delete(listener);
    globalThis.quollTestPopListeners = () => popped.size;
  },

  quoll_test_history_location__deps: ['$stringToUTF8'],
  quoll_test_history_location: (out, size) => {
    stringToUTF8(globalThis.location.pathname + globalThis.location.search, out, size);
  },

  quoll_test_history_back: () => globalThis.history.back(),

  quoll_test_history_listeners: () => globalThis.quollTestPopListeners(),

  // A registry of widgets, as the Vite plugin makes, with one module: a chart that logs what it is asked, and keeps its
  // emit for quoll_test_widgets_emit.
  quoll_test_widgets_install__deps: ['$quollFakeDom'],
  quoll_test_widgets_install: () => {
    quollFakeDom.widgetLog = [];
    quollFakeDom.emit = null;
    quollFakeDom.gate = new Promise((release) => {
      quollFakeDom.release = release;
    });
    const logged = (props) => ({
      update(next) {
        quollFakeDom.widgetLog.push(`update ${JSON.stringify(next)}`);
      },
      unmount() {
        quollFakeDom.widgetLog.push('unmount');
      },
    });

    globalThis.quollWidgets = {
      // Its update and its unmount throw.
      './throwing_chart.js': () =>
        Promise.resolve({
          mount() {
            return {
              update() {
                throw new Error('no update');
              },
              unmount() {
                throw new Error('no unmount');
              },
            };
          },
        }),
      // Its mount waits for quoll_test_widgets_release().
      './async_chart.js': () =>
        Promise.resolve({
          async mount(node, props, emit) {
            await quollFakeDom.gate;
            quollFakeDom.widgetLog.push(`mount ${node.tag} ${JSON.stringify(props)}`);
            quollFakeDom.emit = emit;
            return logged(props);
          },
        }),
      // Its mount waits for quoll_test_widgets_release(), and its unmount throws.
      './async_throwing_chart.js': () =>
        Promise.resolve({
          async mount() {
            await quollFakeDom.gate;

            return {
              unmount() {
                throw new Error('no unmount');
              },
            };
          },
        }),
      // Its mount fails.
      './failing_chart.js': () =>
        Promise.resolve({
          mount() {
            throw new Error('no canvas here');
          },
        }),
      './fake_chart.js': () =>
        Promise.resolve({
          mount(node, props, emit) {
            quollFakeDom.widgetLog.push(`mount ${node.tag} ${JSON.stringify(props)}`);
            quollFakeDom.emit = emit;

            return {
              update(props) {
                quollFakeDom.widgetLog.push(`update ${JSON.stringify(props)}`);
              },
              unmount() {
                quollFakeDom.widgetLog.push('unmount');
              },
            };
          },
        }),
    };
  },

  // What the widgets were asked, one per line, into `out`, of `size` bytes.
  quoll_test_widgets_log__deps: ['$quollFakeDom', '$stringToUTF8'],
  quoll_test_widgets_log: (out, size) => {
    stringToUTF8(quollFakeDom.widgetLog.join('\n'), out, size);
  },

  // What the last widget mounted emits: a function, which JSON.stringify turns into nothing.
  quoll_test_widgets_emit_function__deps: ['$quollFakeDom'],
  quoll_test_widgets_emit_function: () => {
    quollFakeDom.emit('picked', () => {});
  },

  // Lets the mount of ./async_chart.js go on.
  quoll_test_widgets_release__deps: ['$quollFakeDom'],
  quoll_test_widgets_release: () => {
    quollFakeDom.release();
  },

  // What the last widget mounted emits: an object that refers to itself, which JSON cannot hold.
  quoll_test_widgets_emit_circular__deps: ['$quollFakeDom'],
  quoll_test_widgets_emit_circular: () => {
    const value = {};

    value.self = value;
    quollFakeDom.emit('picked', value);
  },

  // What the last widget mounted emits: `name`, and `value`, JSON.
  quoll_test_widgets_emit__deps: ['$quollFakeDom', '$UTF8ToString'],
  quoll_test_widgets_emit: (name, value) => {
    quollFakeDom.emit(UTF8ToString(name), JSON.parse(UTF8ToString(value)));
  },

  // Presses this key in the first input, then submits the first form, as Enter in it does: whether the page kept the
  // browser from leaving it.
  quoll_test_dom_key_and_submit__deps: ['$quollFakeDom', '$UTF8ToString'],
  quoll_test_dom_key_and_submit: (key) => {
    const input = quollFakeDom.root.findTag('input');

    input.listeners.get('keydown')?.({ target: input, key: UTF8ToString(key) });

    const form = quollFakeDom.root.findTag('form');
    const event = {
      target: form,
      currentTarget: form,
      defaultPrevented: false,
      preventDefault() {
        this.defaultPrevented = true;
      },
    };

    form.listeners.get('submit')(event);
    return event.defaultPrevented ? 1 : 0;
  },

  // Presses `key` in the first input, Shift held if `shift`, composing if `composing`, as a browser does.
  quoll_test_dom_key__deps: ['$quollFakeDom', '$UTF8ToString'],
  quoll_test_dom_key: (key, shift, composing) => {
    const target = quollFakeDom.root.findTag('input');

    target.listeners.get('keydown')({ target, key: UTF8ToString(key), shiftKey: shift !== 0, isComposing: composing !== 0 });
  },

  // Submits the first form, as a button in it does: whether the page kept the browser from leaving it.
  quoll_test_dom_submit__deps: ['$quollFakeDom'],
  quoll_test_dom_submit: () => {
    const form = quollFakeDom.root.findTag('form');
    const event = {
      target: form,
      currentTarget: form,
      defaultPrevented: false,
      preventDefault() {
        this.defaultPrevented = true;
      },
    };

    form.listeners.get('submit')(event);
    return event.defaultPrevented ? 1 : 0;
  },

  // Clicks the first checkbox, as a browser does: its property turns, then its change is told.
  quoll_test_dom_check__deps: ['$quollFakeDom'],
  quoll_test_dom_check: () => {
    const target = quollFakeDom.root.findTag('input');

    target.checked = !target.checked;
    target.listeners.get('change')({ target });
  },

  // Whether the first checkbox is checked, as its property says.
  quoll_test_dom_checked__deps: ['$quollFakeDom'],
  quoll_test_dom_checked: () => (quollFakeDom.root.findTag('input').checked ? 1 : 0),

  // Types this text in the first input.
  quoll_test_dom_type__deps: ['$quollFakeDom', '$UTF8ToString'],
  quoll_test_dom_type: (text) => {
    const target = quollFakeDom.root.findTag('input');

    target.value = UTF8ToString(text);
    target.listeners.get('input')({ target });
  },
});
