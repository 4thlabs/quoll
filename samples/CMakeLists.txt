# A command-line program calling GitHub's API: natively, and under Emscripten in Node.
add_subdirectory(github)

# One contract, its server and command line natively, its page in a browser under Emscripten.
add_subdirectory(notes)
