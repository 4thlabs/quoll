# What Quoll costs

The baseline, with bodies in pieces (step 3 of [`PLAN.md`](PLAN.md)), which what follows is compared with. The program is
`tests/bench`:

```bash
cmake -B build-bench -DCMAKE_BUILD_TYPE=Release -DQUOLL_BENCH=ON -DQUOLL_BUILD_TESTS=OFF
cmake --build build-bench --target quoll_bench -j"$(nproc)"
build-bench/tests/bench/quoll_bench tcp              # the first table
build-bench/tests/bench/quoll_bench allocations      # the second
build-bench/tests/bench/quoll_bench serve 8080       # the routes alone, for the third
```

Measured on `d9c3b0f` with the pull request recycling coroutine frames on top, in a Docker container of an Intel Core i5-14400 (16 threads), Linux 6.18, GCC 13.3, on a machine
that also runs other things: each figure is the median of three runs, and a rate moves by some 10 % from one to the next.

## Over loopback TCP

The server and the client are Quoll, each on a thread and a loop of its own, for 5 s after a warm-up of 500 ms. Allocations are
per request, `operator new` only: OpenSSL and the C library allocate past it, and the thread on which asio resolves a name is
not counted. Standard library assertions are off, as in an application, whereas the tests compile with them.

| Scenario | Requests/s | p50 (µs) | p99 (µs) | Allocations, server | Allocations, client |
| --- | --- | --- | --- | --- | --- |
| GET, one connection | 60111 | 13 | 45 | 16 | 10 |
| GET, 16 at once | 121823 | 116 | 312 | 16 | 10 |
| GET, one connection each | 14618 | 42 | 1017 | 39 | 26 |
| POST 1 MiB, echoed | 721 | 1331 | 2189 | 43 | 36 |
| POST 1 MiB, read in pieces | 826 | 1078 | 2419 | 1111 | 26 |
| GET 1 MiB, written in pieces | 644 | 1429 | 3453 | 1046 | 21 |
| REST GET, JSON | 55758 | 15 | 48 | 25 | 16 |
| REST POST, JSON | 14132 | 44 | 1028 | 45 | 35 |
| GET over TLS, one connection | 45757 | 18 | 60 | 16 | 10 |
| GET over TLS, one connection each | 620 | 1557 | 2241 | 42 | 31 |

What it shows, for the performance step:

- A client's POST opens a connection each time, as it may not be sent twice on one found closed: a REST POST costs a connection,
  and JSON costs little beside it (REST GET against GET).
- A new connection has a p99 of about 1 ms against a p50 of 42 µs. Not explained yet.
- Most allocations of a GET are on the server.
- A body in pieces takes about 4 allocations per piece of 4 KiB, all on the side that streams it, against 16 before
  coroutine frames were recycled, when a GET took 54 and 22. Old and new binaries, run in turn on the same machine, kept their
  rates within 6 %: this saves work of the allocator, and not time that shows here.

## Over a memory stream

Both sides on one thread, with no network and with virtual time: the same on every run, in Debug and in Release with GCC 13.
`quoll_bench allocations --check` keeps them within these budgets, in the source of the program, every night in `bench.yml`, which
also writes the first table in its summary. Of the pull requests, only one touching the program or this workflow runs it.

| Scenario | Allocations per request, both sides |
| --- | --- |
| GET, one connection | 22 |
| GET, one connection each | 48 |
| POST 1 MiB, echoed | 63 |
| POST 1 MiB, read in pieces | 821 |
| GET 1 MiB, written in pieces | 800 |
| REST GET, JSON | 37 |
| REST POST, JSON | 64 |

## Beside other servers

To situate the server alone, with a load generator that is not Quoll. The other servers are not in this repository: nginx 1.24 and
h2o 2.2.5 from Ubuntu's packages, in a throwaway container, each on one worker as Quoll is on one thread, nginx answering with
`return 200 "Hello, world!"` and h2o with a file of that text. `wrk 4.1.0`, 5 s, median of three runs.

| Server | 1 connection: requests/s | p50 (µs) | p99 (µs) | 16 connections: requests/s | p50 (µs) | p99 (µs) |
| --- | --- | --- | --- | --- | --- | --- |
| Quoll | 74333 | 11 | 38 | 147940 | 101 | 180 |
| nginx | 91456 | 9 | 27 | 253381 | 59 | 107 |
| h2o | 75641 | 11 | 34 | 244903 | 60 | 130 |

It says what one small response costs, on one thread, and nothing of a real application or of several workers. The headers differ
too: these two send `Date`, `Server` and others that Quoll does not.
