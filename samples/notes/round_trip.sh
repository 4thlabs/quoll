#!/bin/sh
# Starts the notes server, runs the client against it, checks the client's whole session, then stops the server with
# SIGTERM, as a service manager would (a job started in the background of a script ignores SIGINT, and so does the
# runtime then).
#
#   round_trip.sh <quoll_notes_server> <quoll_notes_client> <port>
set -eu

server=$1
client=$2
port=$3
log=$(mktemp)
"$server" "$port" >"$log" 2>&1 &
pid=$!
trap 'kill "$pid" 2>/dev/null || true; rm -f "$log"' EXIT

# Wait until the server listens: the client fails to connect before.
for _ in $(seq 50); do
  if output=$("$client" "http://127.0.0.1:$port" 2>&1); then
    break
  fi
  sleep 0.1
done

printf '%s\n' "$output"

expected='created #1, all the notes:
  #1 buy milk
  #2 write the OpenAPI generator
  #3 buy coffee
the notes about buying:
  #1 buy milk
  #3 buy coffee
removed #1
get #1: 404 no note 1'

if [ "$output" != "$expected" ]; then
  echo "unexpected session; expected:" >&2
  printf '%s\n' "$expected" >&2
  exit 1
fi

kill -TERM "$pid"
wait "$pid"
grep -q '^stopped$' "$log"
