# Notes in a browser

The notes of [`samples/notes`](../README.md) as a page in C++: compiled to WASM, styled with Tailwind and daisyUI, calling
the same contract as the command line. The UI is described in [`docs/ui.md`](../../../docs/ui.md), and the page is written
as it shows.

| File | What it holds |
|---|---|
| `CMakeLists.txt`, `vite.config.js`, `package.json`, `index.html`, `dev.sh` | the build and the development server, with the Vite plugin of [`tools/vite-plugin`](../../../tools/vite-plugin): all that is at the root, the code is in `src/` |
| `src/main.cpp` | `mount(notes_page)`, in `#app` |
| `src/main.js`, `src/style.css` | the page's script, Tailwind and daisyUI |
| `src/components/notes_page` | the notes and what the server answered last, `use_state` hooks, loaded by a `use_effect` on nothing, and the calls that change them |
| `src/components/note_form` | the input of a new note, its draft a state of its own, and the `on_add` callback it calls |
| `src/components/note_row` | one note, with its `on_remove` callback; props compared with `==`, so that a row whose note is unchanged is not viewed again |
| `src/client/notes_client.hpp`, `.cpp` | the calls to the server, through a `rest::client` of the contract, in a source of their own |

Each component is a header, its props and its `extern` declaration, and a source, its view.

## Run it

With Docker, from the root of the repository. The image holds Clang, Emscripten and Node; building it takes some minutes,
once.

```bash
docker build -t quoll-dev .devcontainer

docker run --rm -it -p 5173:5173 -e QUOLL_POLL=1 \
  -v "$PWD":/workspaces/quoll -v quoll-cache:/home/dev/.cache -w /workspaces/quoll \
  quoll-dev samples/notes/web/dev.sh
```

Then open <http://localhost:5173>. The first build compiles everything, for a minute or two. `QUOLL_POLL=1` is for macOS
and Windows, where a shared folder does not always report its changes.

Do not run `npm` on the host in this folder: `node_modules` holds the container's binaries.

In the dev container, or with Emscripten, Node and a C++23 compiler at hand, `samples/notes/web/dev.sh` is enough: it
builds and starts the notes server on port 8080, then Vite on port 5173, which sends `/api` to the server. It runs
[`tools/vite-plugin/dev.sh`](../../../tools/vite-plugin/dev.sh), which the other pages share.

## Try hot reload

Add a note, start typing another one, and change a component in your editor:

| Change | What happens | The note you are typing |
|---|---|---|
| `button("Add")` to `button("Ajouter")`, in `note_form.hpp` | shows at once, without compiling | stays |
| `tw("btn btn-primary")` to `tw("btn btn-secondary")` | shows at once | stays |
| `tw("text-4xl font-bold")` to `tw("text-4xl font-bold text-rose-500")`, in `notes_page.hpp`, a class not used yet | Tailwind generates it, and it shows at once | stays |
| A comment, or spaces | nothing | stays |
| Anything else, such as adding a `state` | a rebuild of about 1.5 s, then the page reloads | **lost** |
| `int x = oops;` | the compiler's first error, over the page | |

## Keep rebuilds short

Each component is a header and a source: a change in one compiles again only its source. Four things keep its
rebuilds short:

- **A source for each component:** a change of a view compiles again only its source, and a change of props its source
  and those placing it.

- **Quoll, precompiled:** `target_precompile_headers` in `CMakeLists.txt`. This only tells the compiler to parse it once.
- **REST and JSON in a source apart:** `src/client/notes_client.cpp`, which compiles only when a call changes. It relays the contract
  by hand for now; it could be generated from it.
- **Line tables, not full debug information:** what the plugin's `debug` option gives by default, `'lines'`: a trace
  still names files and lines. `debug: 'full'` in `vite.config.js` gives the browser's debugger the variables too, for
  rebuilds half as long again; `'none'` gives the shortest.

The time of a rebuild is measured in [the dashboard's README](../../dashboard/README.md), for each of its components.

## Build for production

`npm run build` writes everything to `dist/`, the C++ compiled for size: about 89 KB of gzipped WASM, 6 KB of CSS and 7 KB of
JavaScript.
