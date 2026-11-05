#!/usr/bin/env bash
# Runs every fuzz target, as many at a time as there are cores, and fails if one of them finds a crash.
# usage: run.sh <build directory> <seconds per target> [directory for what crashes] [directory keeping what it finds]
set -u

mkdir -p "${3:-fuzz-findings}" ${4:+"$4"}

build=$(realpath "$1")
seconds=$2
findings=$(realpath "${3:-fuzz-findings}")
corpus=$(realpath "$(dirname "$0")/corpus")
kept=${4:+$(realpath "$4")}

run_target() {
  local name=$1
  local log="$findings/$name.log"
  local work
  local status

  # New inputs go to the first directory in the list: the seeds in the repository stay as they are.
  if [ -n "$kept" ]; then
    work="$kept/$name"
    mkdir -p "$work"
  else
    work=$(mktemp -d)
  fi

  "$build/tests/fuzz/quoll_fuzz_$name" -max_total_time="$seconds" -timeout=10 -rss_limit_mb=2048 \
    -artifact_prefix="$findings/$name-" "$work" "$corpus/$name" >"$log" 2>&1
  status=$?

  if [ -z "$kept" ]; then
    rm -rf "$work"
  fi

  if [ "$status" -ne 0 ]; then
    echo "CRASH: $name"
    cat "$log"
    return 1
  fi

  echo "ok: $name, $(grep -E '^Done ' "$log")"
  rm -f "$log"
}

export build seconds findings corpus kept
export -f run_target

# What was built, not what has seeds: libFuzzer refuses a missing directory, so that a target without any fails here.
targets=""

for path in "$build"/tests/fuzz/quoll_fuzz_*; do
  if [ -f "$path" ] && [ -x "$path" ]; then
    targets="$targets ${path##*/quoll_fuzz_}"
  fi
done

if [ -z "$targets" ]; then
  echo "no fuzz target in $build" >&2
  exit 1
fi

echo "$targets" | tr ' ' '\n' | grep . | xargs -P "$(getconf _NPROCESSORS_ONLN)" -I{} bash -c 'run_target "$1"' _ {}
