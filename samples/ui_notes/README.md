# Notes in a browser

The notes of [`samples/notes`](../notes) as an interface in C++: compiled to WASM, styled with Tailwind and daisyUI, on the
same contract as the server. A prototype: see "UI in C++" in [`docs/PLAN.md`](../../docs/PLAN.md).

```bash
source /path/to/emsdk/emsdk_env.sh
cmake -B build && cmake --build build --target quoll_notes_server && ./build/samples/notes/quoll_notes_server 8080 &
cd samples/ui_notes
npm install
npm run dev        # builds the C++, then serves it on http://localhost:5173
```

While it runs, change `samples/ui_notes/main.cpp`:

| What you change | What happens | State of the page |
|---|---|---|
| The CSS, or a class Tailwind did not generate yet | Vite and Tailwind update the styles | kept |
| A literal of `tw(...)`, `text(...)` or a tag, such as `button("Add")` | sent to the running program, without compiling | kept |
| Anything else | a rebuild, then the page reloads; an error shows in the page | lost |

`npm run build` writes everything to `dist/`, with the program compiled for size.
