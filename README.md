# Quoll

A C++23 web framework aimed at performance and ease of use: a sans-IO async runtime, an HTTP client and server, a typed
REST DSL usable from any program, and a UI DSL compiled to WASM for web, desktop and mobile, styled with Tailwind.

**Status: exploration.** See [`docs/PLAN.md`](docs/PLAN.md) for the target and the roadmap.

## Requirements

- **To build:** C++23, with GCC 13, Clang 19 or Apple Clang 21 at least, and CMake 3.23.
- **In a browser:** Chrome 112, Edge 112, Firefox 121 or Safari 18.2 at least, all there since December 2024, for WebAssembly tail
  calls. Without them, a coroutine resuming another grows the stack, and a long loop of tasks that never suspend overflows it. An
  older browser does not load the module, and the page stays empty.

## License

BSD 3-Clause, see [`LICENSE.md`](LICENSE.md).
