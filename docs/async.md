# Async

`quoll::async` is what Quoll's tasks run on: coroutines that await each other, an executor that resumes them, and the
rules for ending them, a stop included. It does no I/O and has no loop of its own: an adapter brings those (see
[Runtimes](runtimes.md)), and a test runs tasks on `async::run_loop`.

The fragments here assume:

```cpp
namespace async = quoll::async;
namespace http = quoll::http;
```

## Tasks

`async::task<T>` is a coroutine producing a `T`, or nothing for `async::task<>`. It is lazy: it starts when awaited,
and destroying one that never ran does not run it.

```cpp
async::task<user> load_user(int id) {
  auto const response = co_await client.get("/users/" + std::to_string(id));

  co_return parse_user(response.body);
}
```

- **A task is awaited as a temporary**: `co_await load_user(1)`, or `co_await std::move(work)` for one held by name,
  which a `static_assert` asks for. Its frame then belongs to its awaiter, which is the only one to destroy it, once it
  has ended.
- **It ends in one of three ways**: with its value, with an exception, which is thrown again at its `co_await`, or on
  a stop (see [Stopping a task](#stopping-a-task)).
- **It runs on its awaiter's executor**, and only awaits what knows that executor: Quoll's operations, other tasks,
  and what `async::ContextAwaitable` accepts. Anything else does not compile.

## Running a task

From outside a coroutine, `async::co_spawn` runs a task on an executor, never inline, and gives what comes of it to a
handler:

```cpp
struct refreshed {
  void operator()() const { std::cerr << "refreshed\n"; }
  void operator()(std::exception_ptr const &) const { std::cerr << "could not refresh\n"; }
  void operator()(async::stopped_t) const { std::cerr << "stopped before the end\n"; }
};

async::co_spawn(runtime.get_executor(), refresh_cache(), refreshed{}, runtime.get_stop_token());
```

- **A handler has a case for each end**: the value (nothing for a `task<>`), a `std::exception_ptr`, and
  `async::stopped_t`; an overload set of lambdas fits too. One without a case for a stop does not compile: a task that ends on one would have nothing to
  tell.
- **`async::detached`** drops a value, says nothing of a stop, and writes an exception to stderr while the program goes
  on, as tokio does. `async::set_report_sink` sends those lines elsewhere.
- **In a coroutine**, `co_await async::co_spawn(work(), async::detached)` runs it on the coroutine's own executor,
  without waiting for it. It does not take the coroutine's stop token: pass `co_await async::current_stop_token()` for
  it to stop along with it.
- **`runtime.run(task)`** runs the program's main task: see [Getting started](getting-started.md#how-a-program-runs).

## Doing several things

```cpp
using namespace std::chrono_literals;

// At once: both run, and it completes once they have.
auto [second, third] = co_await async::when_all(load_user(2), load_user(3));

// With a deadline: the task is stopped, and awaited to its end.
auto const fourth = co_await async::with_timeout(2s, load_user(4));

co_await async::sleep_for(100ms);
```

| What                     | Ends                                                                                                                                                        |
| ------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `when_all(a, b, ...)`    | Once all have, with their values. A child that throws or ends on a stop stops the others; then the first exception is thrown, or, with none, it ends on the stop. |
| `with_timeout(d, work)`  | With what `work` gives. Past `d`, `work` is stopped and awaited to its end: if it ends on that stop, `std::system_error` with `std::errc::timed_out`.        |
| `sleep_for(d)`           | After `d`, or on a stop.                                                                                                                                     |

`sleep_for` and `with_timeout` need the executor's timer: `quoll::runtime` has one, `async::run_loop` none, and they
throw `std::logic_error` without it.

## Stopping a task

A stop is asked for, never forced: a task stops at what it awaits next, if that looks at the stop. Quoll's operations
do: a sleep, a connection, a read, a write, a wait for a free connection.

### Asking for a stop

A stop goes through a `std::stop_token`, which a chain of tasks shares:

- the token given to `co_spawn`, as `runtime.get_stop_token()`, which the first Ctrl-C stops;
- one a server or a client keeps for its own connections, which its shutdown or a timeout stops;
- `co_await async::current_stop_token()`, for a task to look at it, or hand it to a task it spawns.

### A stop is no exception

When what a task awaits ends on a stop, nothing of the task resumes:

1. what it awaits does not resume it, and no `catch` sees the stop, nor any line after the `co_await`;
2. its frame is destroyed, its locals with it, the `co_await` under way first: what it holds is let go as its
   destructors run;
3. the stop goes up the same way, through every task that awaited it, to the nearest one that asked to see it: a
   `co_spawn` root, a `when_all` child, or `stopped_as_optional`.

So a loop that catches everything and tries again cannot turn a stop into a spin:

```cpp
for (;;) {
  try {
    handle(co_await socket.receive());
  } catch (std::exception const &failure) {
    log(failure.what());  // never a stop: on one, this coroutine does not resume
  }
}
```

C++26's `std::execution` has the same rule, its `set_stopped`, and Rust has it by nature: cancelling a future is
dropping it. Quoll's chains are unwound by their owner, as in Capy.

### Seeing a stop

Where something must still run once stopped, tasks to join or an answer to send, ask for the stop:

```cpp
auto const response = co_await async::stopped_as_optional(client.get("/hello"));

if (!response) {
  std::cerr << "stopped before an answer\n";  // runs: the stop ended get(), not this coroutine
  co_return;
}
```

- **`stopped_as_optional(work)`** gives `std::optional` of what `work` gives (`std::monostate` for a `task<>`), and
  `std::nullopt` once it ended on a stop. It rethrows what `work` throws. It is awaited as a temporary, as a task is.
- **The stop still stands there**: what the coroutine awaits next, if it looks at the stop, ends on it at once. What is
  left to do once stopped is done there, with what does not look at the stop.
- **A destructor cannot `co_await`**: what must be sent on the way out goes through `stopped_as_optional`, not RAII.
- **`with_timeout`** sees the stop of its own timeout, and turns it into `std::errc::timed_out`. A stop of the
  awaiter wins, even with the timer expired meanwhile.

### What a stop does to Quoll's operations

| Operation                                 | On a stop                                                                                                      |
| ----------------------------------------- | -------------------------------------------------------------------------------------------------------------- |
| `sleep_for`, `offload` not started yet    | Ends the awaiter on the stop.                                                                                  |
| A stream's `read_some`, `write_some`      | Ends the awaiter on the stop; a stream does not take a read up again, so the connection is not reused.        |
| `http::client::send`, `stream`            | Abandons the request, closes its connection, and ends the awaiter on the stop.                                 |
| `body.read()`, `event_reader::next()`     | Ends the awaiter on the stop; reading ends there, and a later read throws `std::errc::operation_canceled`.     |
| `http::server::serve`                     | Stops accepting, drains its connections within `shutdown_timeout`, then returns: a stop is how a server ends. |
| A server's handler, a body writer         | Ends on the stop, which is no failure: `on_handler_error` is not told, and no 500 goes out.                    |
| `websocket::receive()`                    | Ends on the stop; receiving ends there, and a later `receive()` throws.                                        |
| A UI handler                              | Ends on the stop; `on_error` is not told, and the app's `stop()` waits for it.                                 |

### Writing your own

- **A task of your own** needs nothing: what it awaits ends on the stop, and so does it.
- **What still throws `std::errc::operation_canceled`** for a stop, code of yours or a library's, ends the task it is
  thrown in on the stop, once it leaves that task, if the task's stop is requested; elsewhere it stays an exception, as
  a failure.
- **A handler of `co_spawn`** of your own has its `async::stopped_t` case; `quoll::test::overloaded`, in Quoll's tests,
  ends the program on a stop it has no case for.

## Threads

A task runs on one thread at a time, its executor's; on a runtime of several threads, the tasks of one connection
take turns on a strand (see [Runtimes](runtimes.md#several-threads)). A stop may be asked for from any thread: the
chain unwinds on its own executor. Hold no lock across a `co_await`: the task may resume on another thread.
