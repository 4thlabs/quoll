# Code review rules

How to review a pull request of this repository, for whoever reviews it.

Read `docs/CODING_RULES.md` and `docs/PLAN.md` first: their rules and decisions are part of what you check. Go through the
whole diff and report everything you find in one review, whatever its severity, the small points too: do not stop at the first
ones, nor keep one back for being minor, as each further round costs a push. Read the code the diff calls and what calls it, as
bugs often sit where changed code meets the rest; outside the diff, report only what the diff causes or exposes. Look for, in this order:

- bugs: undefined behaviour, lifetimes and dangling references (coroutine frames, `_ref` wrappers, `string_view` /
  `span`), unhandled cases, data races, errors swallowed;
- WASM: anything in a core module that requires threads or blocking waits;
- breaks of `docs/CODING_RULES.md`: ODR rules, self-contained headers, naming and layout, concepts instead
  of SFINAE, special members and `noexcept`, type-erasure rules, `detail` namespaces;
- breaks of the decisions in `docs/PLAN.md`;
- missing tests, or tests that do not execute what they claim to test.

In GitHub, Codex only flags P0 and P1. A finding goes out as P1 at least, whatever it is, naming, layout and wording too, and
starts by saying what it really is: `bug:`, `broken rule:`, `missing test:` or `detail:`. The label is no filter here.

Do not repeat a point already answered in a thread. Skip style nits clang-format would catch.
