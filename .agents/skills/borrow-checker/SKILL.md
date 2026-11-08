---
name: borrow-checker
description: A borrow check of C++ code done by hand, asking the questions Rust's compiler asks, to catch use after free, dangling references, use after move, data races and undefined behaviour before any sanitizer runs. Use it before committing or reviewing C++ that holds a reference, pointer, view, span, iterator or `this` beyond one expression, that moves, that suspends (coroutines, callbacks, completion handlers), that destroys objects in an order that matters, or that shares state between threads or tasks, even when nobody asks for a lifetime review.
---

# Borrow checker

C++ compiles code that dangles. Rust refuses it by asking a few questions about every borrow; this skill asks them by hand,
on the diff and on the code it calls. It comes before the tools (ASan, TSan, clang-tidy), which only see the paths a test runs.

## The model

- Every object has **one owner**, which decides when it dies.
- A **borrow** is anything that refers to an object without owning it: a reference, a raw pointer, `std::string_view`,
  `std::span`, an iterator, a non-owning wrapper, a lambda capture by reference, `this` held by a callback or a coroutine frame.
- A borrow never outlives its owner.
- While a borrow is alive, the owner is not changed through another path, unless the borrow itself is the only way to it:
  **shared or mutable, never both**.
- A move ends the source: after it, the object is only assigned or destroyed.

## How to run it

Go through each function the diff adds or changes, and what it calls where the borrow goes. For each borrow, write one line:

| Borrow | Owner | Must live until | Holds because |
|---|---|---|---|

"Holds because" names a reason the code enforces, not one it hopes for: "passed by value", "owned by the frame", "the caller
awaits at once, and the type says so". "The caller keeps it alive" is a reason only for a view or a non-owning type whose name
and documentation say so.

Then do the destruction pass, and go through the checks below. A borrow no check worries about takes one line; a doubt is a
finding.

## The destruction pass

Reviews follow the normal path, and the bugs they miss are on the way out: an object destroyed while still registered
somewhere, or a destructor that calls back into an object half destroyed. Fill these two tables for every class the diff
touches that registers something or owns members whose destruction runs code. An empty cell is a finding.

**Registrations.** Every place that hands a pointer or reference to something that will call it later: a work item put in a
waiter list or a channel, a callback given to a timer, a stop token or an event source, an object put in a registry or a map
owned elsewhere, `this` captured by a scheduled task.

| Registered (where) | Unregistered on completion | on cancellation | on destruction while still registered |
|---|---|---|---|

The last column is the one reviews leave empty. For a coroutine awaitable it means: the frame is destroyed while suspended,
by a parent that stops, a task dropped or a test tearing down. "Nothing destroys it while registered" is a hope, not an entry,
unless the code enforces it. In Quoll it does for a frame: one is destroyed only once it will not resume again, which
`async::task` asserts (`docs/CODING_RULES.md`), so an awaitable that lives in a task's frame enters "the frame outlives it".
The column stays open for everything else: the I/O object, the channel or the timer the awaitable registered with, destroyed
while it is registered.

**Destruction order.** List the members in the order they are destroyed (reverse of declaration, after the destructor's
body), and for each one what its destruction runs: a joined thread, a cancelled task, closed connections, notified observers,
callbacks that reach the object again through some other path.

| Member (destroyed in this order) | What its destruction runs | Members that code touches | Still alive then? |
|---|---|---|---|

Follow the calls: a listener closing its connections, whose close callbacks look the object up in another map, is the case
to find. When what a member's destruction runs is not visible in the code at hand, write "unknown", not "nothing": a member
that owns listeners, connections, tasks, timers or callbacks may run code that reaches the object again. An "unknown" that
could is a doubt to report and a question for whoever knows that code, not a demand for a destructor: an explicit destructor
is needed only where a path back to the object exists. A "no" in the last column is a use after free of a member, which
AddressSanitizer does not see, as the memory is still allocated.

## Checks

### 1. Where does the borrow end up?

A borrow that leaves its expression may outlive what it points to.

- Returned or stored: a reference or view into a parameter, a local or a temporary. `std::string_view sv = make_string();`
  dangles at the semicolon.
- Captured: a lambda capturing by reference (or `this`) that is stored, posted to an executor, given to a thread or a
  `std::function`, or returned.
- Kept by a callee: a constructor or a setter keeping a reference or view it was given.
- A temporary in a range-for initializer, `for (auto &x : make().items())`: do not rely on C++23's lifetime extension
  (P2718), which GCC before 15, among others, does not implement.

### 2. What does the code use after it suspends?

Across a `co_await`, a callback or a completion handler, the caller's statement has ended and its temporaries are gone.

- **Coroutine parameters** by reference or view refer to the caller's objects, which may die at the first suspension: take them
  by value (Core Guidelines CP.53), and the copy lives in the frame. The exception is the object an operation acts on, such as
  the stream a `read(stream, buffer)` reads: it may stay a reference when the caller awaits the operation in the same
  expression, and the parameter is documented and marked lifetimebound.
- **`this`** in a member coroutine or a callback: what keeps the object alive until the frame ends?
- **A lambda that is a coroutine** keeps its captures in the closure, not in the frame (CP.51). It is safe when the closure
  outlives the task, as in `co_await [&]() -> task<> { ... }();`. It is a bug when the task is stored, spawned or returned
  while the closure dies: pass what it needs as parameters instead.
- **Children:** a task started on a local, or given a reference to one, that runs after the parent leaves its scope.
- **Destroyed while suspended:** every awaitable whose `await_suspend` registers something has a row in the registrations
  table of the destruction pass; its last column is the question here. A defaulted destructor leaves a dangling entry. In
  Quoll, a task's frame is not destroyed while suspended (see the destruction pass): the question moves to what the
  awaitable registered with.

### 3. Is the owner changed while a borrow is alive?

- A container modified while an element, an iterator or a view into it is held: `push_back` reallocating, `erase`, `rehash`.
- A function given a view or reference that may point into the object it modifies: `fields.erase(fields.front().name)`. Copy
  first, or take it by value.
- Two parameters that may alias: `T &out` and `T const &in` naming the same object, `a = combine(a, b)`.
- One object reached through two paths, one of them writing.

### 4. Is anything used after a move?

- Read after `std::move`, also in the next iteration of a loop, and in another argument of the same call:
  `f(std::move(x), x.size())`, when `f` takes that parameter by value, may build it before `size()` runs. `std::move` itself
  moves nothing: with a `T &&` parameter, nothing is moved before `f`'s body.
- A member that empties `*this` is `&&`-qualified, so that the call reads `std::move(obj).run()` and a later use shows.
- A callable that may only run once, or an object copied after its contents were moved out.

### 5. Does the destruction order hold?

- Members die in reverse order of declaration: a member whose destructor uses another (a thread, a task, a timer, a registered
  callback) is declared after it.
- A member whose destruction runs code may re-enter the object while it dies: the destruction-order table of the
  destruction pass is where this is followed. With shared ownership, the object dies with its last owner, which may be
  another member.
- A `std::jthread` or a task joined in a destructor uses the members declared before it.
- Do not rely on the order in which coroutine parameters are destroyed, which compilers do not share: put what must outlive the
  task and the task in one struct whose member order says it.
- An exception thrown halfway through a constructor destroys only what was built: no raw handle that a later throw leaks.

### 6. Who else can reach it?

Every object reachable from two threads, or from two tasks on executors that may run in parallel, is one of: immutable, an
atomic, behind a mutex, or confined to one strand. Prefer a type that holds the mutex and the data together and only hands
the data out under the lock, over a mutex member beside the fields it guards. A plain `bool` or flag between threads is a
data race, and so is a `shared_ptr`'s object: only its count is atomic. Ask on which thread each callback and each
resumption runs.

No lock is held across a suspension point (CP.52): the task may resume on another thread, where unlocking a `std::mutex` it
does not own is undefined behaviour, and anything else needing that lock waits for a task that may never resume.

### 7. Can two operations overlap?

An object that allows one operation at a time (a stream, a parser, an awaitable, a writer): what stops a second one from
starting while the first is pending, from awaiting the same awaitable twice, or from moving the object meanwhile? Prefer an API
that makes it hard: the awaitable of one operation with no copy or move and an rvalue-only `operator co_await`, so that the
natural way to await it is where it is made. It is not airtight, as a named one awaited through `std::move` still compiles,
and a task type that has to move, to be stored or spawned, cannot be one. So keep a check that fails in every build as well,
never an assert alone, which release builds drop: it throws where the function may throw, and in a `noexcept` function or a
move it terminates on purpose, or the check moves to an earlier call that may throw.

### 8. Undefined behaviour beyond lifetimes

What safe Rust has none of:

- Signed overflow, and arithmetic on durations or sizes from outside: saturate or check. `size_t` wraps silently.
- Narrowing conversions, including through a template parameter or an implicit conversion to a handler's argument.
- Shifts past the width, division by zero, reading an uninitialized value, an index out of bounds.
- An object in storage less aligned than its type.
- `noexcept` on code that may throw by accident, allocation included: not undefined, but it terminates, which is rarely
  what was meant. A deliberate terminate on misuse, as in check 7, is the exception.
- Text assumed to be UTF-8, or numbers assumed to be finite, when they come from outside.

## Fixing

In this order of preference:

1. **Own it:** copy or move into what outlives the call. A copy costs less than a crash.
2. **Make the API refuse it:** a type, an `&&`-qualifier, a deleted overload, a `static_assert`, so that misuse does not
   compile.
3. **Check it in every build:** throw on a misuse that types cannot express. An invariant that only code speaking the
   library's own protocol can break, out of a normal caller's reach, is asserted instead, and debug builds run the tests:
   a task destroyed while it runs, which needs starting it through `await_suspend`, private to Quoll's own awaiting, is
   one.
4. **Say it:** a name or a view type that says it does not own, the lifetime in the doc comment, and the lifetimebound
   attribute (`[[clang::lifetimebound]]`, behind the project's macro where compilers lack it) on what returns a reference into
   `*this` or a parameter.

Two shapes make the destruction pass's bugs impossible to write:

- **A registration is a token.** What registers returns an object whose destructor unregisters, as `std::stop_callback`
  does, and whoever registered keeps it as a member. Destroying the owner then unregisters on every path. In Quoll, an
  awaitable in a task's frame is not destroyed while it waits (see the destruction pass): what it registered with is.
- **What calls back is taken apart first, on purpose.** When a member's destruction may call back into the object, an
  explicit destructor first clears what a callback could reach (lookups, registrations), then moves that member out into a
  local and destroys it there, while the object is still whole. Declaring it last is not enough: a callback may look it up
  while it is half destroyed, shared ownership moves the real destruction to whichever owner goes last, and the next edit
  that reorders the members brings the bug back without a sound. Declaring a destructor removes the implicit move
  operations: default them back, or define all five special members, as the project's rules ask.

Each real finding gets a test that fails without the fix, under ASan or TSan when it is about memory or threads. For a
registration, the test destroys the object at every state it can be registered in (each suspension point of a coroutine, in
turn), unless that object is a task's frame or an awaitable waiting in it, which Quoll never destroys while suspended: the
test then destroys what it registered with. A member used after its destructor needs MemorySanitizer's use-after-dtor. A flag set by the destructor is no
substitute: reading the object afterwards is itself undefined, and an optimizing compiler may drop the store as dead.

## What to report

In the PR body, a section **Borrow check**: the borrows that needed a reason, one line each, the two tables of the
destruction pass, and what was found and fixed. The tables are the part to keep when a report must be short: they are where
the findings reviews miss come from. Nothing to report: say which functions were checked. In a review, each finding goes
out as the project's review rules ask, like any other.
