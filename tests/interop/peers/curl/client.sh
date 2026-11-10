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

check "GET /hello" sh -c "test \"\$(curl -sS --fail $base/hello)\" = hello"
check "five URLs, one connection" sh -c "curl -sS --fail -o /dev/null -o /dev/null -o /dev/null -o /dev/null -o /dev/null -w '%{num_connects}\n' $base/hello $base/hello $base/hello $base/hello $base/hello | awk '{ connects += \$1 } END { exit !(connects <= 1) }'"
check "POST echoed" sh -c "test \"\$(curl -sS --fail --data-binary ping $base/echo)\" = ping"
check "POST of 2 MiB, Expect: 100-continue" sh -c "head -c 2097152 /dev/zero > /tmp/big && curl -sS --fail -v --expect100-timeout 30 --data-binary @/tmp/big -o /tmp/back $base/echo 2>&1 | grep -q '< HTTP/1.1 100' && cmp /tmp/back /tmp/big"
check "POST chunked" sh -c "test \"\$(printf abc | curl -sS --fail -H 'Transfer-Encoding: chunked' --data-binary @- $base/echo)\" = abc"
check "a body in pieces" sh -c "test \$(curl -sS --fail $base/pieces/1000 | wc -l) -eq 1000"
check "server-sent events" sh -c "test \$(curl -sS --fail -N $base/events/5 | grep -c '^data: event ') -eq 5"
check "HTTP/1.0" sh -c "test \"\$(curl -sS --fail -0 $base/hello)\" = hello"
check "HEAD" sh -c "curl -sS --fail -I $base/hello | grep -qi '^content-length: 5'"
check "a target not found is 404" sh -c "test \$(curl -sS -o /dev/null -w '%{http_code}' $base/nowhere) = 404"

exit $((failures > 0))
