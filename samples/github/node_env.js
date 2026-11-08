// Linked with --pre-js into quoll_github under Emscripten: the variables the program reads, from Node's environment
// into Emscripten's, which is its own. Those two only: nothing else of the environment reaches the program.
Module['preRun'] = Module['preRun'] || [];
Module['preRun'].push(() => {
  for (const name of ['GITHUB_TOKEN', 'QUOLL_GITHUB_URL']) {
    if (process.env[name] !== undefined) {
      ENV[name] = process.env[name];
    }
  }
});
