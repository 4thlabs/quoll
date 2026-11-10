#!/bin/sh
# Quoll against third-party clients and servers: HTTP, server-sent events and WebSocket, each way, on a Docker network.
#
#   tests/interop/run.sh <directory of quoll_interop_server and quoll_interop_client> [what...]
#
# what: curl python node nginx autobahn-server (third-party clients of Quoll's server, nginx as its proxy), node-server
# python-server autobahn-client (Quoll's client against third-party servers); all by default.
#
# QUOLL_RUN runs a Quoll program where the containers reach it as QUOLL_HOST, on the network QUOLL_NETWORK:
# by default, `docker exec quoll-work`, the container of the development machine, joined to the network as "quoll".
# The third-party programs only ever run in containers. Reports (Autobahn's) go to build-interop/ in the repository.

bin=$1
shift
what=${*:-curl python node nginx autobahn-server node-server python-server autobahn-client}
here=$(cd "$(dirname "$0")" && pwd)
peers=$here/peers
out=${QUOLL_INTEROP_OUT:-$here/../../build-interop}
network=${QUOLL_NETWORK:-quoll-interop}
run=${QUOLL_RUN:-docker exec quoll-work}
host=${QUOLL_HOST:-quoll}
port=18080
failed=""

mkdir -p "$out"
docker network inspect "$network" >/dev/null 2>&1 || docker network create "$network" >/dev/null

say() { printf '\n== %s\n' "$*"; }
fail() { failed="$failed $1"; }

# Quoll's server, for the third-party clients.
start_quoll_server() {
  stop_quoll_server
  $run sh -c "'$bin/quoll_interop_server' $port >/tmp/quoll_interop_server.log 2>&1 & echo \$! >/tmp/quoll_interop_server.pid"
  sleep 1
}

# By its process id, as an image may have no pkill; SIGTERM, which a shell's background job does not ignore as SIGINT.
stop_quoll_server() {
  $run sh -c 'test -f /tmp/quoll_interop_server.pid && kill -TERM "$(cat /tmp/quoll_interop_server.pid)" 2>/dev/null;
              rm -f /tmp/quoll_interop_server.pid' || true
}

# A third-party server, in a container named `name` on the network, until stop_peer.
start_peer() {
  name=$1
  shift
  docker rm -f "$name" >/dev/null 2>&1
  docker run -d --name "$name" --network "$network" "$@" >/dev/null
}

stop_peer() { docker rm -f "$1" >/dev/null 2>&1; }

# Waits for http://name:port/ to answer from the network.
wait_for() {
  for turn in $(seq 1 120); do
    if docker run --rm --network "$network" alpine:3 sh -c "nc -z $1 $2" >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  echo "$1:$2 never answered"
  return 1
}

# Autobahn's report, its summary: each case's behaviour, and those that failed.
autobahn_summary() {
  docker run --rm -v "$out:/reports" python:3.12-slim python3 -c "
import json, sys, collections
index = json.load(open('/reports/$1/index.json'))
counts = collections.Counter()
bad = []
for agent, cases in index.items():
    for case, result in cases.items():
        counts[result['behavior']] += 1
        if result['behavior'] not in ('OK', 'NON-STRICT', 'INFORMATIONAL', 'UNIMPLEMENTED'):
            bad.append((case, result['behavior'], result['behaviorClose']))
print(dict(counts))
for case, behavior, close in sorted(bad, key=lambda b: [int(x) for x in b[0].split('.')]):
    print('  ', case, behavior, close)
sys.exit(1 if bad else 0)
"
}

for one in $what; do
  case $one in
    curl)
      say "curl -> Quoll's server"
      start_quoll_server
      docker run --rm --network "$network" -e QUOLL_URL="http://$host:$port" -v "$peers:/peers:ro" alpine:3 \
        sh -c 'apk add -q curl >/dev/null && sh /peers/curl/client.sh' || fail curl
      ;;
    python)
      say "Python (requests, httpx, http.client, websockets) -> Quoll's server"
      start_quoll_server
      docker run --rm --network "$network" -e QUOLL_URL="http://$host:$port" -v "$peers:/peers:ro" python:3.12-slim \
        sh -c 'pip install -q --root-user-action=ignore -r /peers/python/requirements.txt 2>/dev/null && python3 /peers/python/client.py' \
        || fail python
      ;;
    node)
      say "Node (fetch, WebSocket) -> Quoll's server"
      start_quoll_server
      docker run --rm --network "$network" -e QUOLL_URL="http://$host:$port" -v "$peers:/peers:ro" node:24-slim \
        node /peers/node/client.mjs || fail node
      ;;
    nginx)
      say "Quoll's client -> nginx -> Quoll's server"
      start_quoll_server
      start_peer quoll-nginx -v "$peers/nginx/nginx.conf:/etc/nginx/nginx.conf:ro" nginx:1.27-alpine
      wait_for quoll-nginx 8080 || fail nginx
      $run "$bin/quoll_interop_client" http http://quoll-nginx:8080 || fail nginx-http
      $run "$bin/quoll_interop_client" sse http://quoll-nginx:8080 || fail nginx-sse
      $run "$bin/quoll_interop_client" ws ws://quoll-nginx:8080/ws || fail nginx-ws
      stop_peer quoll-nginx
      ;;
    autobahn-server)
      say "Autobahn's fuzzing client -> Quoll's WebSocket server"
      start_quoll_server
      docker run --rm --network "$network" -v "$peers/autobahn:/config:ro" -v "$out:/reports" \
        crossbario/autobahn-testsuite wstest -m fuzzingclient -s /config/fuzzingclient.json >/dev/null 2>&1
      autobahn_summary server || fail autobahn-server
      ;;
    node-server)
      say "Quoll's client -> Node's server (node:http, ws)"
      start_peer quoll-node-server -v "$peers/node:/peers:ro" node:24-slim \
        sh -c 'cp -r /peers /srv/app && cd /srv/app && npm install --silent >/dev/null 2>&1 && node server.mjs'
      wait_for quoll-node-server 8080 || fail node-server
      $run "$bin/quoll_interop_client" http http://quoll-node-server:8080 || fail node-server-http
      $run "$bin/quoll_interop_client" sse http://quoll-node-server:8080 || fail node-server-sse
      $run "$bin/quoll_interop_client" ws ws://quoll-node-server:8080/ws || fail node-server-ws
      stop_peer quoll-node-server
      ;;
    python-server)
      say "Quoll's client -> Python's server (aiohttp)"
      start_peer quoll-python-server -v "$peers/python:/peers:ro" python:3.12-slim \
        sh -c 'pip install -q --root-user-action=ignore -r /peers/requirements.txt 2>/dev/null && python3 /peers/server.py'
      wait_for quoll-python-server 8080 || fail python-server
      $run "$bin/quoll_interop_client" http http://quoll-python-server:8080 || fail python-server-http
      $run "$bin/quoll_interop_client" sse http://quoll-python-server:8080 || fail python-server-sse
      $run "$bin/quoll_interop_client" ws ws://quoll-python-server:8080/ws || fail python-server-ws
      stop_peer quoll-python-server
      ;;
    autobahn-client)
      say "Quoll's WebSocket client -> Autobahn's fuzzing server"
      start_peer quoll-autobahn -v "$peers/autobahn:/config:ro" -v "$out:/reports" crossbario/autobahn-testsuite \
        wstest -m fuzzingserver -s /config/fuzzingserver.json
      wait_for quoll-autobahn 9001 || fail autobahn-client
      $run "$bin/quoll_interop_client" autobahn ws://quoll-autobahn:9001 >"$out/autobahn-client.log" 2>&1
      autobahn_summary client || fail autobahn-client
      stop_peer quoll-autobahn
      ;;
    *)
      echo "unknown: $one"
      fail "$one"
      ;;
  esac
done

stop_quoll_server

if [ -n "$failed" ]; then
  say "failed:$failed"
  exit 1
fi

say "all passed"
