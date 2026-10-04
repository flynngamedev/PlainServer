#!/usr/bin/env bash
# Verifies that:
#   1. every PlS:: command declared in runtime/include/pls/pls_runtime.h has
#      exactly one C++ definition in the command source files
#   2. the same set of names (and count) appears in the Rust arity table
#      (compiler/src/pls_commands.rs), which codegen uses to validate call
#      sites before emitting C++
#
# Why this exists: PlainS's grammar treats `Namespace::name(...)` fully
# generically, so nothing forces the header, the C++ definitions, and the
# Rust table to agree. A command missing from the Rust table still compiles
# (with a warning) and still links; a command missing a definition doesn't
# fail until the linker, with a message naming a mangled symbol. This turns
# both into one clear report.
#
# Run from anywhere; paths are resolved relative to this script.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

# Only the files that implement PlS:: commands are scanned. pls_value.cpp,
# pls_json.cpp, net.cpp, and builtins.cpp deliberately contain no commands
# -- they are the value layer, the wire format, and the bare-name builtins
# -- so scanning them would report every one of their functions as an
# undeclared command. Listing the command files explicitly (rather than
# excluding by name) means a NEW command file has to be added here
# consciously, instead of being silently skipped.
COMMAND_SOURCES="src/server.cpp src/world.cpp src/physics.cpp src/broadcast.cpp src/debug.cpp"
cd runtime

declared=$(grep -oP '^Value \K[A-Za-z_][A-Za-z0-9_]*' include/pls/pls_runtime.h | sort -u)
defined_raw=$(grep -hoP '^Value \K[A-Za-z_][A-Za-z0-9_]*(?=\()' $COMMAND_SOURCES | sort)

# Internal helpers that live in a command file but are not commands.
# Kept short and explicit: a real command that is merely undeclared is
# exactly the bug this script exists to catch, so this must never become a
# pattern broad enough to swallow one.
# File-local helpers are marked `static` by convention so they never appear
# in this scan at all; only shared helpers with external linkage need listing.
#   serializeEntity -- shared entity->packet helper (declared in pls_server.h)
internal_helpers='^(serializeEntity)$'
defined=$(echo "$defined_raw" | grep -vE "$internal_helpers" | sort -u)

# Only the PLS_COMMANDS block, not BUILTINS: the bare-name builtins are
# intentionally absent from pls_runtime.h (they live in pls::builtin, see
# builtins.cpp), so including them here would report all fifteen as
# undeclared commands. The block is sliced by line range BEFORE extracting
# names, since a name on its own carries no indication of which table it
# came from.
rust_names=$(sed -n '/pub const PLS_COMMANDS/,/^];/p' ../compiler/src/pls_commands.rs |
  grep -oP 'sig!\("\K[A-Za-z_][A-Za-z0-9_]*' | sort -u)

fail=0

missing_defs=$(comm -23 <(echo "$declared") <(echo "$defined"))
if [ -n "$missing_defs" ]; then
  echo "MISSING C++ definitions for declared commands:"
  echo "$missing_defs"
  fail=1
fi

extra_defs=$(comm -13 <(echo "$declared") <(echo "$defined"))
if [ -n "$extra_defs" ]; then
  echo "C++ functions defined but not declared in pls_runtime.h (typo?):"
  echo "$extra_defs"
  fail=1
fi

dupes=$(echo "$defined_raw" | grep -vE "$internal_helpers" | uniq -d)
if [ -n "$dupes" ]; then
  echo "Commands with more than one C++ definition:"
  echo "$dupes"
  fail=1
fi

missing_rust=$(comm -23 <(echo "$declared") <(echo "$rust_names"))
if [ -n "$missing_rust" ]; then
  echo "MISSING Rust arity table entries (compiler/src/pls_commands.rs) for:"
  echo "$missing_rust"
  fail=1
fi

extra_rust=$(comm -13 <(echo "$declared") <(echo "$rust_names"))
if [ -n "$extra_rust" ]; then
  echo "Rust arity table has entries with no matching C++ declaration:"
  echo "$extra_rust"
  fail=1
fi

count=$(echo "$declared" | wc -l | tr -d ' ')
if [ "$fail" -eq 0 ]; then
  echo "OK: all $count PlS:: commands have exactly one C++ definition and a matching Rust arity entry."
else
  exit 1
fi
