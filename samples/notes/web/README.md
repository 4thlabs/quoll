# Notes in a browser

The notes of [`samples/notes`](../README.md) as a page in C++: compiled to WASM, styled with Tailwind and daisyUI, calling
the same contract as the command line. The UI is described in [`docs/ui.md`](../../../docs/ui.md), and the page is written
as it shows.

| File | What it holds |
|---|---|
| `main.cpp` | `mount<notes_page>()`, in `#app` |
| `components/notes_page.hpp` | the notes, what the server answered last, and the calls that change them |
| `components/note_form.hpp` | the input of a new note, its draft a state of its own, and the `on_add` callback it calls |
| `components/note_row.hpp` | one note, with its `on_remove` callback; props compared with `==`, so that a row whose note is unchanged is not viewed again |
| `notes_client.hpp`, `.cpp` | the calls to the server, through a `rest::client` of the contract, in a source of their own |
| `src/app.js`, `src/style.css`, `index.html` | what Vite serves: the program's script, Tailwind and daisyUI |
| `vite.config.js` | the Vite plugin of [`tools/vite-plugin`](../../../tools/vite-plugin), with the target and its sources |

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
builds and starts the notes server on port 8080, then Vite on port 5173, which sends `/api` to the server.

## Try hot reload

Add a note, start typing another one, and change a component in your editor:

| Change | What happens | The note you are typing |
|---|---|---|
| `button("Add")` to `button("Ajouter")`, in `note_form.hpp` | shows at once, without compiling | stays |
| `tw("btn btn-primary")` to `tw("btn btn-secondary")` | shows at once | stays |
| `tw("text-4xl font-bold")` to `tw("text-4xl font-bold text-rose-500")`, in `notes_page.hpp`, a class not used yet | Tailwind generates it, and it shows at once | stays |
| A comment, or spaces | nothing | stays |
| Anything else, such as adding a `state` | a rebuild of about a second, then the page reloads | **lost** |
| `int x = oops;` | the compiler's first error, over the page | |

## Keep rebuilds short

The page lives in headers, with no source of its own but `main.cpp`. Two things keep its rebuilds short:

- **Quoll, precompiled:** `target_precompile_headers` in `CMakeLists.txt`. This only tells the compiler to parse it once.
- **REST and JSON in a source apart:** `notes_client.cpp`, which compiles only when a call changes. It relays the contract
  by hand for now; it could be generated from it.

Measured from a save to the end of the build, for a change in the page, on one machine with 8 cores: 3.9 s with everything
in one source and nothing precompiled, about 0.8 s with these two.

## Build for production

`npm run build` writes everything to `dist/`, the C++ compiled for size: about 89 KB of gzipped WASM, 6 KB of CSS and 7 KB of
JavaScript.
