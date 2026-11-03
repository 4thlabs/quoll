# Adapters plug the core modules into a runtime (threads, asio...). Each one is a module of its own, quoll::<adapter>,
# so that an application only pulls the adapters it uses and their dependencies.
add_subdirectory(threads)
add_subdirectory(asio)
add_subdirectory(emscripten)
