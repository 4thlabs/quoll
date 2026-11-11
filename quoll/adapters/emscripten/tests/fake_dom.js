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

      setAttribute(name, value) {
        this.attributes.set(name, value);
      }

      removeAttribute(name) {
        this.attributes.delete(name);
      }

      addEventListener(name, listener) {
        this.listeners.set(name, listener);
      }

      removeEventListener(name, listener) {
        if (this.listeners.get(name) === listener) {
          this.listeners.delete(name);
        }
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

  // A registry of widgets, as the Vite plugin makes, with one module: a chart that logs what it is asked, and keeps its
  // emit for quoll_test_widgets_emit.
  quoll_test_widgets_install__deps: ['$quollFakeDom'],
  quoll_test_widgets_install: () => {
    quollFakeDom.widgetLog = [];
    quollFakeDom.emit = null;
    globalThis.quollWidgets = {
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

  // What the last widget mounted emits: `name`, and `value`, JSON.
  quoll_test_widgets_emit__deps: ['$quollFakeDom', '$UTF8ToString'],
  quoll_test_widgets_emit: (name, value) => {
    quollFakeDom.emit(UTF8ToString(name), JSON.parse(UTF8ToString(value)));
  },

  // Types this text in the first input.
  quoll_test_dom_type__deps: ['$quollFakeDom', '$UTF8ToString'],
  quoll_test_dom_type: (text) => {
    const target = quollFakeDom.root.findTag('input');

    target.value = UTF8ToString(text);
    target.listeners.get('input')({ target });
  },
});
