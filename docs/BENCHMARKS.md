# What Quoll costs

The baseline before streaming (step 3 of [`PLAN.md`](PLAN.md)), which what follows is compared with. The program is
`tests/bench`:

```bash
cmake -B build-bench -DCMAKE_BUILD_TYPE=Release -DQUOLL_BENCH=ON -DQUOLL_BUILD_TESTS=OFF
cmake --build build-bench --target quoll_bench -j"$(nproc)"
build-bench/tests/bench/quoll_bench tcp              # the first table
build-bench/tests/bench/quoll_bench allocations      # the second
build-bench/tests/bench/quoll_bench serve 8080       # the routes alone, for the third
```

Measured on commit `272e663`, in a Docker container of an Intel Core i5-14400 (16 threads), Linux 6.18, GCC 13.3, on a machine
that also runs other things: each figure is the median of three runs, and a rate moves by some 10 % from one to the next.

## Over loopback TCP

The server and the client are Quoll, each on a thread and a loop of its own, for 5 s after a warm-up of 500 ms. Allocations are
per request, `operator new` only: OpenSSL and the C library allocate past it, and the thread on which asio resolves a name is
not counted. Standard library assertions are off, as in an application, whereas the tests compile with them.

| Scenario | Requests/s | p50 (µs) | p99 (µs) | Allocations, server | Allocations, client |
| --- | --- | --- | --- | --- | --- |
| GET, one connection | 69331 | 13 | 37 | 54 | 22 |
| GET, 16 at once | 129490 | 115 | 295 | 54 | 22 |
| GET, one connection each | 14392 | 38 | 1004 | 110 | 49 |
| POST 1 MiB, echoed | 879 | 1109 | 1565 | 107 | 59 |
| REST GET, JSON | 64629 | 14 | 40 | 64 | 29 |
| REST POST, JSON | 14206 | 40 | 964 | 108 | 59 |
| GET over TLS, one connection | 53095 | 17 | 53 | 55 | 22 |
| GET over TLS, one connection each | 908 | 1065 | 1669 | 116 | 55 |

What it shows, for the performance step:

- A client's POST opens a connection each time, as it may not be sent twice on one found closed: a REST POST costs a connection,
  and JSON costs little beside it (REST GET against GET).
- A new connection has a p99 of about 1 ms against a p50 of 38 µs. Not explained yet.
- Most allocations of a GET are on the server.

## Over a memory stream

Both sides on one thread, with no network and with virtual time: the same on every run, in Debug and in Release with GCC 13.
`quoll_bench allocations --check` keeps them within these budgets, in the source of the program, every night in `bench.yml`, which
also writes the first table in its summary. Of the pull requests, only one touching the program or this workflow runs it.

| Scenario | Allocations per request, both sides |
| --- | --- |
| GET, one connection | 72 |
| GET, one connection each | 136 |
| POST 1 MiB, echoed | 144 |
| REST GET, JSON | 89 |
| REST POST, JSON | 145 |

## Beside other servers

To situate the server alone, with a load generator that is not Quoll. The other servers are not in this repository: nginx 1.24 and
h2o 2.2.5 from Ubuntu's packages, in a throwaway container, each on one worker as Quoll is on one thread, nginx answering with
`return 200 "Hello, world!"` and h2o with a file of that text. `wrk 4.1.0`, 5 s, median of three runs.

| Server | 1 connection: requests/s | p50 (µs) | p99 (µs) | 16 connections: requests/s | p50 (µs) | p99 (µs) |
| --- | --- | --- | --- | --- | --- | --- |
| Quoll | 80656 | 11 | 35 | 149778 | 104 | 159 |
| nginx | 100475 | 9 | 24 | 266499 | 58 | 101 |
| h2o | 80309 | 11 | 30 | 255803 | 59 | 130 |

It says what one small response costs, on one thread, and nothing of a real application or of several workers. The headers differ
too: these two send `Date`, `Server` and others that Quoll does not.
