#!/bin/sh
# Starts the fake GitHub, runs quoll_github against it, checks what it prints and how it ends, then stops the fake with
# SIGTERM.
#
#   against_fake.sh <quoll_fake_github> <quoll_github>
set -eu

fake=$1
program=$2
log=$(mktemp)
out=$(mktemp)
"$fake" 0 >"$log" 2>&1 &
pid=$!
trap 'kill "$pid" 2>/dev/null || true; rm -f "$log" "$out"' EXIT

fail() {
  echo "$1; what the program printed:" >&2
  cat "$out" >&2
  echo "the fake's output:" >&2
  cat "$log" >&2
  exit 1
}

for _ in $(seq 50); do
  kill -0 "$pid" 2>/dev/null || fail "the fake ended before listening"
  grep -q '^fake GitHub on ' "$log" && break
  sleep 0.1
done

url=$(sed -n 's/^fake GitHub on //p' "$log")
[ -n "$url" ] || fail "the fake did not start"

# A repository and its open issues, through a scope; the fake refuses a request without the User-Agent the
# program's interceptor adds.
QUOLL_GITHUB_URL=$url "$program" 4thlabs/quoll >"$out" 2>&1 || fail "4thlabs/quoll failed"
grep -q '^4thlabs/quoll: 42 stars, 2 open issues$' "$out" || fail "no repository"
grep -q '^  #7 SSE (ada)$' "$out" || fail "no issue"

# A repository that is not there: GitHub's error, declared, through try_call.
if QUOLL_GITHUB_URL=$url "$program" 4thlabs/nothing >"$out" 2>&1; then
  fail "a missing repository succeeded"
fi
grep -q '^not found: Not Found$' "$out" || fail "no error for a missing repository"

# The account of a token, and none without one.
QUOLL_GITHUB_URL=$url GITHUB_TOKEN=test-token "$program" me >"$out" 2>&1 || fail "me failed"
grep -q '^ada (Ada Lovelace)$' "$out" || fail "no account"

if QUOLL_GITHUB_URL=$url "$program" me >"$out" 2>&1; then
  fail "me succeeded without a token"
fi

echo "quoll_github against the fake GitHub: as expected"
