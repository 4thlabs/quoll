# The life of a component

How the app makes a component, views it, runs its effects and lets it go, drawn from the code of `quoll::ui`. The
diagrams use the names of the functions that do each step, so that each can be read in its file. What a component is and
how one is written are in [the UI guide](ui.md).

- [The states of an instance](#the-states-of-an-instance)
- [A first placement, through its first load shown](#a-first-placement-through-its-first-load-shown)
- [New props, then the removal](#new-props-then-the-removal)
- [A component written as a function](#a-component-written-as-a-function)
- [Where it runs](#where-it-runs)

## The states of an instance

```mermaid
stateDiagram-v2
    [*] --> Placed: the parent's view calls it
    Placed --> Making: no instance at its place
    Placed --> Kept: an instance at its place
    Making --> [*]: making throws, as a shared context missing
    Making --> Made: made, members joined, first effects
    Made --> Viewing
    Kept --> GivenAgain: props equal, no state changed
    Kept --> Viewing: props differ, or a state changed
    Viewing --> Recorded: its view returned
    Viewing --> Discarded: its view threw
    Recorded --> Shown: committed
    Recorded --> Tried: the render failed after the view
    Tried --> Viewing: next render
    Discarded --> [*]: an instance just made is dropped
    GivenAgain --> Shown
    Shown --> Shown: effects started, if it was viewed
    Shown --> Dirty: one of its states changed
    Dirty --> Kept: the next render
    Shown --> Unmounted: its parent's view no longer has it
    Unmounted --> Draining: stop asked, its runs unwinding
    Draining --> Destroyed: its last owner lets it go
    Destroyed --> [*]
```

- **Placed.** A call such as `product_card({.id = 3})` makes an element holding the component's table and its props
  (`hooks_element`, `component.hpp`; `tag<C>::operator()`, `tag.hpp`). Nothing is made at the call.
- **Its place.** The app finds the instance of the last render by the component's type and its key, or its rank among
  those of its type in the parent's view without a key (`matching::take`, `src/view/expand.cpp`). Two of one type with
  one key in a view throw.
- **Making** (`instance::instance`, `src/component/instance.cpp`): its props kept, its table's `make`, its stop following
  the app's, `bind` with the contexts above it, `link` to its instance, then `first_effects`. A struct is built in
  `bind` (`struct_box::make`, `detail/component/struct_table.hpp`) under a `making_scope`: each member's `member_join`
  joins the struct's `member_registry` as it is built, and an effect following the props finds where they are kept
  (`making_props_of`). `first_effects` tells the targets whose first run will start that they are loading, before the
  first view.
- **Given again** (`walk_component`, `expand.cpp`): an instance whose states have not changed since its last view, with
  props equal by `==` to the last ones, gets its last view again, and starts no effect.
- **Viewing** (`run_view`): a struct's props are replaced by the new ones, and restored if its view throws
  (`view_struct`); its view runs, its components walked in turn.
- **Shown** (`app_core::render`, `src/app/app_core.cpp`): the reconciler applies the differences to the renderer, then
  the instances are committed (`viewed`), linked to the app if just made, and those left behind unmounted. A failure
  past the views discards the render (`discard`): the instances just made go, the others keep their last view.
- **Effects** (`start_effects`): after the render, those of each instance that was viewed, children before parents, then
  those of the contexts.
- **Dirty** (`instance::changed_in`): a state's change marks its instance and asks the app for a render
  (`state_changed`), posted once for every change of a turn (`render_later`).
- **Unmounted** (`instance::unmount`): its children first, then its stop asked, then unlinked. Its runs end on that stop,
  writing nothing; the instance goes with the last task that holds it (`~instance`), its members in reverse order, each
  leaving the registry.

## A first placement, through its first load shown

A component written as a struct whose `resource` follows `&props::id`:

```mermaid
sequenceDiagram
    participant Parent as parent's view
    participant Core as app_core
    participant Expand as expand
    participant Inst as instance
    participant Struct as struct_box
    participant Load as effect
    participant Exec as executor
    Parent->>Core: product_card(props)
    Core->>Expand: render, expand
    Expand->>Expand: matching take finds none
    Expand->>Inst: instance made
    Inst->>Struct: make, then bind under a making_scope
    Struct->>Load: built, member_join joins the registry
    Inst->>Load: first_effects, after_view first: target loading
    Expand->>Struct: run_view, view_struct, view()
    Core->>Core: reconciler applies, commit
    Core->>Load: start_effects, after_view: record differs
    Load->>Exec: spawn the run
    Exec->>Load: run_in: begin, the function's task
    Load->>Load: its task ends: target finish, state update
    Load->>Inst: changed_in
    Inst->>Core: state_changed, render_later
    Exec->>Core: render: the instance kept, viewed again
    Core->>Core: the view shows what it loaded
```

- **The first run** starts after the first render is shown, as React runs an effect once its render is committed. Its
  target said it was loading before that view, so the first frame shows it loading (`effect::after_view`,
  `src/data/effect.cpp`).
- **What a run reads** of the props is read at that view, by its field or its function, and copied into the run
  (`props_reader`, `data/effect.hpp`): the run holds no reference into props that the next view replaces.
- **Its end** writes its target at once, which tells the instance, which asks for a render
  (`run_in`, `detail/data/run_slot.hpp`; `write_quietly`, `detail/data/target.hpp`).

## New props, then the removal

```mermaid
sequenceDiagram
    participant Parent as parent instance
    participant Core as app_core
    participant Inst as product_card instance
    participant Load as effect
    participant Before as run before
    participant Exec as executor
    Parent->>Core: a state changed, render_later
    Core->>Inst: the parent's view gives new props
    Inst->>Inst: kept, props differ: view_struct with the new ones
    Core->>Inst: commit
    Core->>Load: after_view: record differs
    Load->>Before: ask: its stop requested
    Load->>Exec: spawn the new run
    Before-->>Before: stopped: writes nothing
    Parent->>Core: later, its view no longer places it
    Core->>Inst: commit: unmount, children first, stop, unlink
    Exec->>Load: run_in: its caller stopped, the target loads no more
    Exec->>Core: its task ends, it holds the instance no more
    Core->>Inst: the last owner goes: destroyed
    Inst->>Load: destroyed with the struct, leaving the registry
```

- **A change** of what an effect follows stops the run before at once (`ask`, `src/data/run_slot.cpp`): a run whose answer
  comes after writes nothing, even one that ignored the stop.
- **What it follows holding none**, as an optional, stops the run before; a `resource` then holds nothing loaded
  (`target_.release()`).
- **The removal** asks its stop and waits for its runs to end; a run's frame is destroyed once it will not resume, by
  its root (`docs/CODING_RULES.md`). The instance outlives its tasks: each holds it until it ends.

## A component written as a function

The same steps, with these differences:

- **Its hooks** live in a `hook_store`, empty when the instance is made, filled by its first view: each `use_` call is
  found again by where it is called, its file, line and column, and its rank among the calls from there
  (`hook_store::find`, `next_call`, `src/component/hook_store.cpp`). A call that finds a hook of another type throws.
- **An effect made in the first view** tells its target that it is loading during that view
  (`use_effect_at`, `component/hooks.hpp`): the app views it once more at the end of that render, so the first frame
  shown holds the flag.
- **A run calls the function of the view that asked for it**, once that view is shown, with the props it was given
  (`effect_hook`, `hooks.hpp`).

## Where it runs

- **One executor**: the app's views, its effects, its handlers and its actions run on the executor it was given, one
  thing at a time (`app_core.hpp`). Natively, an executor of a runtime of several threads is a strand.
- **A render** is posted: every change of a turn asks for one render, which views every instance from the root and gives
  the others their last view again.
- **A failure**: a view that throws fails the render, which shows nothing new, and the app's `on_error` hears of it; in
  `app::start()`, it stops the app. A run that fails writes its target's error, and the app's `on_error` hears of it.
