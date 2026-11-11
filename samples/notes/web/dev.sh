#!/bin/bash
# Everything the sample needs in development, in one command: the notes server, then Vite, which builds the C++, serves it
# and reloads it. Meant for the dev container: see README.md. Ctrl-C stops both.
set -euo pipefail

sample="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$sample/../../.." && pwd)"

# The cache volume of the dev container starts out owned by root.
if [ ! -w "$HOME/.cache" ] && command -v sudo >/dev/null; then
  sudo chown "$(id -u):$(id -g)" "$HOME/.cache"
fi

if [ -f /opt/emsdk/emsdk_env.sh ]; then
  EMSDK_QUIET=1 source /opt/emsdk/emsdk_env.sh
fi

echo "== the notes server"
cmake -S "$root" -B "$root/build-native" -DCMAKE_BUILD_TYPE=Debug -DQUOLL_BUILD_TESTS=OFF -DQUOLL_TLS=OFF >/dev/null
cmake --build "$root/build-native" --target quoll_notes_server >/dev/null

"$root/build-native/samples/notes/quoll_notes_server" 8080 &
server=$!
trap 'kill "$server" 2>/dev/null || true' EXIT

cd "$sample"

if [ ! -d node_modules ]; then
  echo "== npm"
  npm ci --silent
fi

# The first build compiles all of it, for a minute or two; then Vite prints its address.
echo "== the C++ and Vite"
npx vite --host 0.0.0.0 --port 5173 --strictPort
