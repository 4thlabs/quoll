#!/bin/sh
# Starts the fake GitHub, runs quoll_github against it, checks what it prints and how it ends, then stops the fake with
# SIGTERM.
#
#   against_fake.sh <quoll_fake_github> <quoll_github>
set -eu

fake=$1
program=$2
# A token of the caller's would change what the program does.
unset GITHUB_TOKEN
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
# program's interceptor adds, answers 503 to the first request for issues, which its retry sends again, and lists the
# issues only for the query the program sends.
QUOLL_GITHUB_URL=$url "$program" 4thlabs/quoll >"$out" 2>&1 || fail "4thlabs/quoll failed"
grep -q '^4thlabs/quoll: 42 stars, 2 open issues$' "$out" || fail "no repository"
grep -q '^  #7 SSE (ada)$' "$out" || fail "no issue"
grep -q '^  #9 WebSocket (grace) \[pull request\]$' "$out" || fail "no pull request"

# GitHub's errors, declared, through try_call.
if QUOLL_GITHUB_URL=$url "$program" 4thlabs/nothing >"$out" 2>&1; then
  fail "a missing repository succeeded"
fi
grep -q '^not found: Not Found$' "$out" || fail "no error for a missing repository"

if QUOLL_GITHUB_URL=$url "$program" 4thlabs/private >"$out" 2>&1; then
  fail "a private repository succeeded"
fi
grep -q '^forbidden: Resource not accessible$' "$out" || fail "no error for a private repository"

# The token goes to every request, where GitHub takes it, not only to /user: a private repository, with access.
QUOLL_GITHUB_URL=$url GITHUB_TOKEN=test-token "$program" 4thlabs/private >"$out" 2>&1 || fail "4thlabs/private failed"
grep -q '^4thlabs/private: 1 stars, 0 open issues$' "$out" || fail "no private repository with a token"

# The account of a token, a token refused with GitHub's body, and none at all.
QUOLL_GITHUB_URL=$url GITHUB_TOKEN=test-token "$program" me >"$out" 2>&1 || fail "me failed"
grep -q '^ada (Ada Lovelace)$' "$out" || fail "no account"

if QUOLL_GITHUB_URL=$url GITHUB_TOKEN=wrong "$program" me >"$out" 2>&1; then
  fail "me succeeded with a wrong token"
fi
grep -q '^the token is refused: Bad credentials$' "$out" || fail "no error for a wrong token"

if QUOLL_GITHUB_URL=$url "$program" me >"$out" 2>&1; then
  fail "me succeeded without a token"
fi
grep -q '^no token: set GITHUB_TOKEN$' "$out" || fail "no error without a token"

echo "quoll_github against the fake GitHub: as expected"
