# Performance

What Quoll costs, and how to read the figures. They are all in [`BENCHMARKS.md`](BENCHMARKS.md), with
their tables and how they were measured: an Intel Core i5-14400 in a Docker container, GCC 13, on a
machine that also runs other things. A rate moves by some 10 % from one run to the next.

Tuning comes last on [the roadmap](PLAN.md#step-9-performance), on purpose: these figures are a
baseline to compare against, not a finished result.

## What to expect

**A small request.** With a Quoll server and a Quoll client over loopback TCP, each on one thread, a GET
takes a median of 13 µs on one connection, at some 60,000 requests a second, and 116 µs on 16 at once,
at some 120,000.

**Beside other servers.** Under a load generator that is not Quoll, serving one small response on one
thread, Quoll is level with h2o and some 20 % behind nginx on one connection, and some 40 % behind both
on 16, with about 148,000 requests a second against about 250,000. This says what one small response
costs, and nothing of a real application or of several workers. Those two also send `Date`, `Server` and
other fields that Quoll does not.

**What costs more:**

- **A new connection.** A request on a connection of its own is several times slower than on a kept one.
  A POST opens one every time, as [it may not be sent twice](client.md#keep-alive-and-retries) on a
  connection found closed, which is most of what a REST POST costs.
- **JSON** costs little beside that: a REST GET stays close to a plain GET.
- **TLS** costs its handshake: close to plain HTTP on a kept connection, and tens of times slower with a
  connection each.
- **Bodies in pieces** take about 4 allocations per piece of 4 KiB on the side that streams them, and run
  at about the same rate as a body read whole.

Allocations are per request, counted over `operator new`. A GET on one connection takes 26 of them for
both sides over TCP, most of them on the server.

**To compile.** A program using `quoll::runtime` compiles neither llhttp nor asio. A native server
rebuilt in 3.9 s and a page in 0.81 s, for 52 KB of gzipped WebAssembly with the REST client, at the end
of [step 1](PLAN.md#step-1-compiled-libraries-done).

## Larger than memory

A body in pieces costs the same whatever its size: 32 GiB each way, a request, a response and a file, at some 1.2 GiB/s over
loopback, with both sides in one process that never held more than 10.3 MiB. See
[`BENCHMARKS.md`](BENCHMARKS.md#larger-than-memory) and [Streaming](streaming.md).

## How it is kept honest

`tests/bench` holds the program, `quoll_bench`:

```bash
cmake -B build-bench -DCMAKE_BUILD_TYPE=Release -DQUOLL_BENCH=ON -DQUOLL_BUILD_TESTS=OFF
cmake --build build-bench --target quoll_bench -j"$(nproc)"
build-bench/tests/bench/quoll_bench tcp              # rates and latency, over loopback
build-bench/tests/bench/quoll_bench allocations      # allocations, over a memory stream
build-bench/tests/bench/quoll_bench large 32         # bodies far larger than memory
build-bench/tests/bench/quoll_bench serve 8080       # the routes alone, for a load generator
```

Over a memory stream, with virtual time, allocations are the same on every run, in Debug and in Release.
So `quoll_bench allocations --check` holds them to budgets written in its source, every night in CI, and
on a pull request touching the program. A change that raises a budget says why.

`QUOLL_BENCH` turns the standard library's assertions off, as an application builds, whereas the tests
compile with them: measure with this build rather than with the tests'.
