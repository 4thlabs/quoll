#!/bin/sh
# Quoll against third-party clients and servers: HTTP, server-sent events and WebSocket, each way, on a Docker network.
#
#   tests/interop/run.sh <directory of quoll_interop_server and quoll_interop_client> [what...]
#
# what: curl python node nginx autobahn-server (third-party clients of Quoll's server, nginx as its proxy), node-server
# python-server autobahn-client (Quoll's client against third-party servers); all by default.
#
# Quoll's programs run in the container QUOLL_CONTAINER, quoll-work by default, which the script joins to the network
# QUOLL_NETWORK, quoll-interop by default, as "quoll", the name the peers and their configurations reach it by. The
# third-party programs only ever run in containers. Autobahn's reports go to build-interop/ in the repository.

bin=$1
shift
what=${*:-curl python node nginx autobahn-server node-server python-server autobahn-client}
here=$(cd "$(dirname "$0")" && pwd)
peers=$here/peers
out=${QUOLL_INTEROP_OUT:-$here/../../build-interop}
network=${QUOLL_NETWORK:-quoll-interop}
container=${QUOLL_CONTAINER:-quoll-work}
run="docker exec $container"
port=18080
# Autobahn's release, and the cases it holds but for 12 and 13 (permessage-deflate), which each report must hold.
autobahn=crossbario/autobahn-testsuite:25.10.1@sha256:519915fb568b04c9383f70a1c405ae3ff44ab9e35835b085239c258b6fac3074
autobahn_cases=301
failed=""

mkdir -p "$out"
docker network inspect "$network" >/dev/null 2>&1 || docker network create "$network" >/dev/null
# Joined as "quoll", unless it already is, as a container started on the network is.
docker network connect --alias quoll "$network" "$container" 2>/dev/null || true

say() { printf '\n== %s\n' "$*"; }
# Each section once, however many of its steps failed.
fail() { case " $failed " in *" $1 "*) ;; *) failed="$failed $1" ;; esac; }

# A run, bounded, so that one that hangs fails rather than holds the rest: within the container, where the program runs.
in_quoll() { $run timeout 600 "$@"; }
in_peer() { timeout 660 docker run --rm --init --network "$network" "$@"; }

# Waits for host:port to answer from the network, in one container that tries for two minutes.
wait_for() {
  docker run --rm --network "$network" alpine:3 sh -c \
    "for turn in \$(seq 1 240); do nc -z $1 $2 && exit 0; sleep 0.5; done; exit 1" >/dev/null 2>&1 && return 0
  echo "$1:$2 never answered"
  return 1
}

# By its process id, as an image may have no pkill; SIGTERM, which a shell's background job does not ignore as SIGINT.
# Waited for, as it drains first: its port is free once it is gone.
stop_quoll_server() {
  $run sh -c 'test -f /tmp/quoll_interop_server.pid || exit 0
              pid=$(cat /tmp/quoll_interop_server.pid)
              kill -TERM "$pid" 2>/dev/null
              for turn in $(seq 1 120); do kill -0 "$pid" 2>/dev/null || break; sleep 0.5; done
              rm -f /tmp/quoll_interop_server.pid' || true
}

start_quoll_server() {
  stop_quoll_server
  $run sh -c "'$bin/quoll_interop_server' $port >/tmp/quoll_interop_server.log 2>&1 & echo \$! >/tmp/quoll_interop_server.pid"

  if ! wait_for quoll "$port"; then
    $run cat /tmp/quoll_interop_server.log
    return 1
  fi
}

# A third-party server, in a container named `name` on the network, until stop_peer.
start_peer() {
  name=$1
  shift
  docker rm -f "$name" >/dev/null 2>&1
  docker run -d --init --name "$name" --network "$network" "$@" >/dev/null
}

stop_peer() { docker rm -f "$1" >/dev/null 2>&1; }

# Autobahn's reports of a former run, which its container wrote as root: removed from a container too.
clear_reports() { docker run --rm -v "$out:/reports" alpine:3 rm -rf "/reports/$1"; }

# Autobahn's report, its summary: each case's behaviour, those that failed, and whether it holds every case, as one that
# never reached Autobahn is missing from it. No report, or one cut short, is a failure.
autobahn_summary() {
  docker run --rm -v "$out:/reports" python:3.12-slim python3 -c "
import json, sys, collections, os
if not os.path.exists('/reports/$1/index.json'):
    sys.exit('   no report')
try:
    index = json.load(open('/reports/$1/index.json'))
except ValueError as error:
    sys.exit(f'   a report cut short: {error}')
counts = collections.Counter()
bad = []
for agent, cases in index.items():
    for case, result in cases.items():
        counts[result['behavior']] += 1
        # The close's verdict too, which a case may fail on its own.
        fine = ('OK', 'NON-STRICT', 'INFORMATIONAL', 'UNIMPLEMENTED')
        if result['behavior'] not in fine or result['behaviorClose'] not in fine:
            bad.append((case, result['behavior'], result['behaviorClose']))
print(dict(counts))
miscounted = sum(counts.values()) != $autobahn_cases
if miscounted:
    print('  ', sum(counts.values()), 'cases in the report, $autobahn_cases expected')
for case, behavior, close in sorted(bad, key=lambda b: [int(x) for x in b[0].split('.')]):
    print('  ', case, behavior, close)
sys.exit(1 if bad or miscounted else 0)
"
}

for one in $what; do
  case $one in
    curl)
      say "curl -> Quoll's server"
      start_quoll_server || { fail curl; continue; }
      in_peer -e QUOLL_URL="http://quoll:$port" -v "$peers:/peers:ro" alpine:3 \
        sh -c 'apk add -q curl >/dev/null && sh /peers/curl/client.sh' || fail curl
      ;;
    python)
      say "Python (requests, httpx, http.client, websockets) -> Quoll's server"
      start_quoll_server || { fail python; continue; }
      in_peer -e QUOLL_URL="http://quoll:$port" -v "$peers:/peers:ro" python:3.12-slim \
        sh -c 'pip install -q --root-user-action=ignore -r /peers/python/requirements.txt 2>/dev/null && python3 /peers/python/client.py' \
        || fail python
      ;;
    node)
      say "Node (fetch, http, WebSocket) -> Quoll's server"
      start_quoll_server || { fail node; continue; }
      in_peer -e QUOLL_URL="http://quoll:$port" -v "$peers:/peers:ro" node:24-slim \
        node /peers/node/client.mjs || fail node
      ;;
    nginx)
      say "Quoll's client -> nginx -> Quoll's server"
      start_quoll_server || { fail nginx; continue; }
      start_peer quoll-nginx -v "$peers/nginx/nginx.conf:/etc/nginx/nginx.conf:ro" nginx:1.27-alpine
      if wait_for quoll-nginx 8080; then
        in_quoll "$bin/quoll_interop_client" http http://quoll-nginx:8080 || fail nginx-http
        in_quoll "$bin/quoll_interop_client" sse http://quoll-nginx:8080 || fail nginx-sse
        in_quoll "$bin/quoll_interop_client" ws ws://quoll-nginx:8080/ws || fail nginx-ws
      else
        fail nginx
      fi
      stop_peer quoll-nginx
      ;;
    autobahn-server)
      say "Autobahn's fuzzing client -> Quoll's WebSocket server"
      start_quoll_server || { fail autobahn-server; continue; }
      clear_reports server
      # The summary whatever the run's end, which says what it did run.
      in_peer -v "$peers/autobahn:/config:ro" -v "$out:/reports" \
        $autobahn wstest -m fuzzingclient -s /config/fuzzingclient.json >"$out/autobahn-server.log" 2>&1 ||
        { tail -20 "$out/autobahn-server.log"; fail autobahn-server; }
      autobahn_summary server || fail autobahn-server
      ;;
    node-server)
      say "Quoll's client -> Node's server (node:http, ws)"
      start_peer quoll-node-server -v "$peers/node:/peers:ro" node:24-slim \
        sh -c 'cp -r /peers /srv/app && cd /srv/app && npm install --silent >/dev/null 2>&1 && node server.mjs'
      if wait_for quoll-node-server 8080; then
        in_quoll "$bin/quoll_interop_client" http http://quoll-node-server:8080 || fail node-server-http
        in_quoll "$bin/quoll_interop_client" sse http://quoll-node-server:8080 || fail node-server-sse
        in_quoll "$bin/quoll_interop_client" ws ws://quoll-node-server:8080/ws || fail node-server-ws
      else
        fail node-server
      fi
      stop_peer quoll-node-server
      ;;
    python-server)
      say "Quoll's client -> Python's server (aiohttp)"
      start_peer quoll-python-server -v "$peers/python:/peers:ro" python:3.12-slim \
        sh -c 'pip install -q --root-user-action=ignore -r /peers/requirements.txt 2>/dev/null && python3 /peers/server.py'
      if wait_for quoll-python-server 8080; then
        in_quoll "$bin/quoll_interop_client" http http://quoll-python-server:8080 || fail python-server-http
        in_quoll "$bin/quoll_interop_client" sse http://quoll-python-server:8080 || fail python-server-sse
        in_quoll "$bin/quoll_interop_client" ws ws://quoll-python-server:8080/ws || fail python-server-ws
      else
        fail python-server
      fi
      stop_peer quoll-python-server
      ;;
    autobahn-client)
      say "Quoll's WebSocket client -> Autobahn's fuzzing server"
      clear_reports client
      if ! start_peer quoll-autobahn -v "$peers/autobahn:/config:ro" -v "$out:/reports" $autobahn \
        wstest -m fuzzingserver -s /config/fuzzingserver.json; then
        echo "Autobahn's fuzzing server did not start"
        fail autobahn-client
      elif wait_for quoll-autobahn 9001; then
        in_quoll "$bin/quoll_interop_client" autobahn ws://quoll-autobahn:9001 >"$out/autobahn-client.log" 2>&1 ||
          { tail -20 "$out/autobahn-client.log"; fail autobahn-client; }
        autobahn_summary client || fail autobahn-client
      else
        docker logs --tail 20 quoll-autobahn
        fail autobahn-client
      fi
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
