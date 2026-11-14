#!/bin/sh
# Starts the fake Docker on a socket of its own, runs quoll_docker against it, checks what it prints and how it ends,
# then stops the fake with SIGTERM.
#
#   against_fake.sh <quoll_fake_docker> <quoll_docker>
set -eu

fake=$1
program=$2
dir=$(mktemp -d)
socket=$dir/docker.sock
log=$dir/fake.log
out=$dir/out
"$fake" "$socket" >"$log" 2>&1 &
pid=$!
trap 'kill "$pid" 2>/dev/null || true; rm -rf "$dir"' EXIT

fail() {
  echo "$1; what the program printed:" >&2
  cat "$out" >&2
  echo "the fake's output:" >&2
  cat "$log" >&2
  exit 1
}

for _ in $(seq 50); do
  kill -0 "$pid" 2>/dev/null || fail "the fake ended before listening"
  grep -q '^fake Docker on ' "$log" && break
  sleep 0.1
done

export DOCKER_HOST="unix://$socket"

"$program" version >"$out" 2>&1 || fail "version failed"
grep -q '^Docker 24.0.7 (API 1.43), linux/amd64$' "$out" || fail "no version"

# A container run to its end, removed: its exit code is the program's.
"$program" run alpine echo hi >"$out" 2>&1 || fail "run failed"
grep -q ' exited with 0$' "$out" || fail "no exit code"

if "$program" run alpine false >"$out" 2>&1; then
  fail "a failing command succeeded"
fi
grep -q ' exited with 1$' "$out" || fail "no failing exit code"

# A filter of Docker's, JSON in the query: run removed what it ran, so none has exited.
"$program" ps -f status=exited >"$out" 2>&1 || fail "ps -f failed"
[ ! -s "$out" ] || fail "an exited container listed, which run removed"

# Nothing left of them.
"$program" ps -a >"$out" 2>&1 || fail "ps failed"
[ ! -s "$out" ] || fail "containers left"

# Docker's errors, in its own shape, through try_call.
if "$program" run missing >"$out" 2>&1; then
  fail "a missing image succeeded"
fi
grep -q '^error 404: No such image: missing:latest$' "$out" || fail "no error for a missing image"

if "$program" inspect nothing >"$out" 2>&1; then
  fail "a missing container succeeded"
fi
grep -q '^error 404: No such container: nothing$' "$out" || fail "no error for a missing container"

echo "quoll_docker against the fake Docker: as expected"
