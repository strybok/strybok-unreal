#!/usr/bin/env bash
# Builds and runs the tests of the engine-independent core. Unreal Engine is not needed.
#
#   Tests/run_core_tests.sh [output-dir]
#
# output-dir defaults to a fresh temporary directory, so nothing is written into the repository.
# CXX selects the compiler (default: clang++, then g++).
#
# The core is built twice, as C++17 and as C++20 (Unreal 5.3+ compiles modules as C++20), with
# exceptions and RTTI switched off and shadowing treated as an error, like Unreal does.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
core="$here/../Source/Strybok/Private/StrybokCore"
out="${1:-$(mktemp -d "${TMPDIR:-/tmp}/strybok_core_tests.XXXXXX")}"
mkdir -p "$out"

cxx="${CXX:-}"
if [ -z "$cxx" ]; then
  if command -v clang++ >/dev/null 2>&1; then cxx=clang++; else cxx=g++; fi
fi

flags=(-O1 -g -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Wshadow -Wundef -Wconversion -Werror)
# Sanitizers catch use-after-free in the re-entrancy tests. Set STRYBOK_NO_SANITIZE=1 to skip.
if [ -z "${STRYBOK_NO_SANITIZE:-}" ]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi

status=0
for std in c++17 c++20; do
  bin="$out/strybok_core_tests_${std//+/p}"
  echo "== $cxx -std=$std -> $bin"
  "$cxx" "-std=$std" "${flags[@]}" -I "$core" "$core/StrybokCore.cpp" "$here/core_tests.cpp" -o "$bin"
  "$bin" || status=$?
done
exit "$status"
