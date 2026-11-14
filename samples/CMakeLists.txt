# A command-line program calling the Docker Engine over its Unix domain socket, natively.
add_subdirectory(docker)

# A command-line program calling GitHub's API: natively, and under Emscripten in Node.
add_subdirectory(github)

# One contract, its server and command line natively, its page in a browser under Emscripten.
add_subdirectory(notes)

# A screen of an application: a list to filter, a detail to edit and a chart.js chart, over its own contract.
add_subdirectory(dashboard)
