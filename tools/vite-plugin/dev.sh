#!/bin/bash
# Everything a page needs in development, in one command: its API server, built natively and run on port 8080, then Vite
# on port 5173, which builds the C++, serves it and reloads it, and sends /api to the server. Ctrl-C stops both.
#
#   tools/vite-plugin/dev.sh <the page's directory> <the server's target>
#
# The server's program is found in build-native, at the place of its target's directory in the sources.
set -euo pipefail

page="$(cd "$1" && pwd)"
server_target="$2"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# The cache volume of the dev container starts out owned by root.
if [ ! -w "$HOME/.cache" ] && command -v sudo >/dev/null; then
  sudo chown "$(id -u):$(id -g)" "$HOME/.cache"
fi

if [ -f /opt/emsdk/emsdk_env.sh ]; then
  EMSDK_QUIET=1 source /opt/emsdk/emsdk_env.sh
fi

echo "== the server, $server_target"
cmake -S "$root" -B "$root/build-native" -DCMAKE_BUILD_TYPE=Debug -DQUOLL_BUILD_TESTS=OFF -DQUOLL_TLS=OFF >/dev/null
cmake --build "$root/build-native" --target "$server_target" >/dev/null

# The page's sample holds its server: samples/<sample>/web is served by build-native/samples/<sample>/<target>. In bash
# rather than realpath, whose macOS version has no --relative-to.
sample="$(dirname "${page#"$root"/}")"
server="$root/build-native/$sample/$server_target"

"$server" 8080 &
server_pid=$!
trap 'kill "$server_pid" 2>/dev/null || true' EXIT

cd "$page"

if [ ! -d node_modules ]; then
  echo "== npm"
  npm ci --silent
fi

# The first build compiles all of it, for a minute or two; then Vite prints its address.
echo "== the C++ and Vite"
npx vite --host 0.0.0.0 --port 5173 --strictPort
