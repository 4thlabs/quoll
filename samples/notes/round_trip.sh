#!/bin/sh
# Starts the notes server on a free port, runs the client against it, then stops the server with SIGINT.
#
#   round_trip.sh <quoll_notes_server> <quoll_notes_client>
set -eu

server=$1
client=$2
log=$(mktemp)
"$server" 0 >"$log" 2>&1 &
pid=$!
trap 'kill "$pid" 2>/dev/null || true; rm -f "$log"' EXIT

# The server prints its port once it listens.
port=
for _ in $(seq 100); do
  port=$(sed -n 's|^notes API on http://localhost:\([0-9]*\)/.*|\1|p' "$log")
  [ -n "$port" ] && break
  sleep 0.1
done

if [ -z "$port" ]; then
  echo "the server did not start:" >&2
  cat "$log" >&2
  exit 1
fi

output=$("$client" 127.0.0.1 "$port")
printf '%s\n' "$output"
printf '%s\n' "$output" | grep -q '^get #1: 404 no note 1$'

kill -INT "$pid"
wait "$pid"
grep -q '^stopped$' "$log"
