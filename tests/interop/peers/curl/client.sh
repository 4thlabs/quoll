#!/bin/sh
# curl against Quoll's server, at QUOLL_URL.
base=$QUOLL_URL
failures=0

check() {
  name=$1
  shift

  if out=$("$@" 2>&1); then
    echo "ok $name"
  else
    failures=$((failures + 1))
    echo "FAIL $name: $out"
  fi
}

# curl, bounded, its body saved to /tmp/got, read only once curl has succeeded: piped out of curl, or in a $(...) within
# a test, the status of a response cut short would be lost.
fetch() { curl -sS --fail -m 30 -o /tmp/got "$@"; }

# /tmp/got against what comes in, byte for byte, shown when it differs.
same() { cmp -s - /tmp/got || { echo "got:"; head -c 300 /tmp/got; false; }; }

# The body of curl's answer to "$@" after the first, which is the body expected.
body_is() {
  expected=$1
  shift
  fetch "$@" && printf %s "$expected" | same
}

pieces() { fetch "$base/pieces/$1" && seq 0 $(($1 - 1)) | sed 's/^/piece /' | same; }

events() {
  fetch -N "$base/events/$1" && for index in $(seq 0 $(($1 - 1))); do
    printf 'event: count\nid: %s\ndata: event %s\n\n' "$index" "$index"
  done | same
}

chunked() { printf abc | fetch -H 'Transfer-Encoding: chunked' --data-binary @- "$base/echo" && printf abc | same; }

# Content-Length 5, the length of GET's body, and nothing more after it.
head_answer() { fetch -I "$base/hello" && { tr -d '\r' < /tmp/got | grep -qix 'content-length: 5' || { cat /tmp/got; false; }; }; }

not_found() { curl -sS -m 30 -o /dev/null -w '%{http_code}' "$base/nowhere" > /tmp/got && printf 404 | same; }

five_on_one() {
  curl -sS --fail -m 30 -o /dev/null -o /dev/null -o /dev/null -o /dev/null -o /dev/null \
    -w '%{http_code} %{num_connects}\n' "$base/hello" "$base/hello" "$base/hello" "$base/hello" "$base/hello" > /tmp/connects &&
    awk '{ answered += ($1 == 200); connects += $2 } END { exit !(NR == 5 && answered == 5 && connects == 1) }' /tmp/connects ||
    { echo 'status and connections opened, per URL:'; cat /tmp/connects; false; }
}

# After a 100, the body of 2 MiB sent and echoed; curl's trace shown on a failure.
continued() {
  head -c 2097152 /dev/zero > /tmp/big &&
    curl -sS --fail -m 60 -v --expect100-timeout 30 --data-binary @/tmp/big -o /tmp/back "$base/echo" 2>/tmp/trace &&
    grep -q '< HTTP/1.1 100' /tmp/trace && cmp /tmp/back /tmp/big || { tail -20 /tmp/trace; false; }
}

check "GET /hello" body_is hello "$base/hello"
check "five URLs, one connection" five_on_one
check "POST echoed" body_is ping --data-binary ping "$base/echo"
check "POST of 2 MiB, Expect: 100-continue" continued
check "POST chunked" chunked
check "a body in pieces" pieces 1000
check "server-sent events" events 5
check "HTTP/1.0" body_is hello -0 "$base/hello"
check "HEAD" head_answer
check "a target not found is 404" not_found

exit $((failures > 0))
