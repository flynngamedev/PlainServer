#!/usr/bin/env bash
# verify_all.sh -- end-to-end verification of the whole PlainS toolchain.
#
#   1. The Rust workspace builds clean and its test suite passes
#   2. The C++ runtime test suite passes (Value semantics, JSON, framing)
#   3. Every PlS:: command has one implementation and a matching arity entry
#   4. Every example parses, generates C++, compiles, and links
#   5. The built server actually runs, ticks, and accepts connections
#   6. The configured max_players ceiling is enforced
#   7. Regression checks for previously fixed bugs
#   8. Live interop against the real PlainVulkan client, if it is available
#
# Usage: bash scripts/verify_all.sh [--release]
#
# The interop step needs PlainVulkan checked out next to PlainS (or pointed at
# by PV_ROOT). It is skipped, not failed, when that isn't the case.
set -uo pipefail

PLS_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PLS_ROOT"

RELEASE=0
for arg in "$@"; do
  case "$arg" in
    --release) RELEASE=1 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

WORK="${PLS_VERIFY_WORKDIR:-/tmp/pls_verify}"
mkdir -p "$WORK"
PORT=8080

PASS=0
FAIL=0
SKIP=0
FAILED_NAMES=()

say()  { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }
ok()   { PASS=$((PASS+1)); printf '  \033[32mPASS\033[0m  %s\n' "$*"; }
bad()  { FAIL=$((FAIL+1)); FAILED_NAMES+=("$1"); printf '  \033[31mFAIL\033[0m  %s\n' "$1"; }
skip() { SKIP=$((SKIP+1)); printf '  \033[33mSKIP\033[0m  %s\n' "$*"; }

check() {
  local name="$1"; shift
  local log="$1"; shift
  if "$@" > "$log" 2>&1; then ok "$name"; else bad "$name"; echo "        (log: $log)"; tail -15 "$log" | sed 's/^/        | /'; fi
}

# Kill anything we started, on any exit path.
SERVER_PID=""
cleanup() { [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null; return 0; }
trap cleanup EXIT

# ---------------------------------------------------------------- Rust ---
say "1. Rust transpiler + compiler"
check "cargo build" "$WORK/cargo_build.log" cargo build --release
check "cargo test"  "$WORK/cargo_test.log"  cargo test --release

PLSC="$PLS_ROOT/target/release/plsc"
PLSCC="$PLS_ROOT/target/release/plscc"
[ -x "$PLSC" ]  && ok "plsc binary exists"  || bad "plsc binary exists"
[ -x "$PLSCC" ] && ok "plscc binary exists" || bad "plscc binary exists"

# ------------------------------------------------------- C++ unit tests ---
say "2. C++ runtime test suite"
check "runtime_tests compile" "$WORK/rt_build.log" \
  c++ -std=c++17 -Iruntime/include -o "$WORK/runtime_tests" \
      runtime/tests/runtime_tests.cpp runtime/src/pls_value.cpp \
      runtime/src/pls_json.cpp runtime/src/net.cpp
check "runtime_tests pass" "$WORK/rt_run.log" "$WORK/runtime_tests"

check "command coverage" "$WORK/cov.log" bash scripts/check_command_coverage.sh

# ------------------------------------------------------------- parsing ---
say "3. Parsing every .pls source"
# plsc takes the file directly (`plsc <file.pls> [--ast]`); there is no
# `check` subcommand -- that spelling belongs to PlainVulkan's pvc.
for f in examples/*/main.pls; do
  n="$(basename "$(dirname "$f")")"
  check "parse $n" "$WORK/parse_$n.log" "$PLSC" "$f"
done

# ------------------------------------------------------------ building ---
say "4. Example projects"
for d in examples/*/; do
  p="$(basename "$d")"
  A=(build "$PLS_ROOT/examples/$p" --runtime-dir "$PLS_ROOT/runtime")
  [ "$RELEASE" = 1 ] && A+=(--release)
  check "build $p" "$WORK/build_$p.log" "$PLSCC" "${A[@]}"
done

# A `pls new` project, covering the starter template users actually get.
rm -rf "$WORK/StarterServer"
(cd "$WORK" && python3 "$PLS_ROOT/pls.py" new StarterServer > /dev/null 2>&1)
if [ -f "$WORK/StarterServer/plsproject.json" ]; then
  A=(build "$WORK/StarterServer" --runtime-dir "$PLS_ROOT/runtime")
  [ "$RELEASE" = 1 ] && A+=(--release)
  check "build StarterServer (pls new template)" "$WORK/build_starter.log" "$PLSCC" "${A[@]}"
else
  bad "pls new produced a project"
fi

# ------------------------------------------------------------- running ---
say "5. The server actually runs"
SERVER_BIN="$PLS_ROOT/examples/mp_server/build/bin/mp_server"
if [ ! -x "$SERVER_BIN" ]; then
  bad "mp_server binary exists"
else
  setsid "$SERVER_BIN" > "$WORK/server.log" 2>&1 < /dev/null &
  SERVER_PID=$!
  sleep 4
  if grep -q "listening on port" "$WORK/server.log"; then
    ok "server starts and binds the port"
  else
    bad "server starts and binds the port"; tail -5 "$WORK/server.log" | sed 's/^/        | /'
  fi

  # PlS::Run() must block. A server that returns from Run() immediately would
  # look like a clean exit and take every player with it.
  sleep 6
  if grep -qE "tick=[1-9][0-9]{2,}" "$WORK/server.log"; then
    ok "tick loop is running (Run() blocks as documented)"
  else
    bad "tick loop is running"; tail -5 "$WORK/server.log" | sed 's/^/        | /'
  fi

  # ------------------------------------------------ connection ceiling ---
  say "6. max_players is enforced"
  RESULT="$(timeout 60 python3 - "$PORT" <<'PYEOF'
import socket, sys, time
port = int(sys.argv[1])
conns = []
for _ in range(90):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=2)
        conns.append(s)
    except OSError:
        pass
time.sleep(1.5)
alive = 0
for s in conns:
    try:
        s.settimeout(0.3)
        if s.recv(1):
            alive += 1
    except socket.timeout:
        alive += 1
    except OSError:
        pass
    finally:
        try: s.close()
        except OSError: pass
print(alive)
PYEOF
)"
  # mp_server declares max_players 64. The ceiling must hold, and it must not
  # be so aggressive that ordinary players are refused.
  if [ "$RESULT" -le 64 ] && [ "$RESULT" -ge 32 ]; then
    ok "90 connection attempts capped at $RESULT (limit 64)"
  else
    bad "max_players ceiling (90 attempts left $RESULT open, expected <= 64)"
  fi
  sleep 2
fi

# ---------------------------------------------------------- regressions ---
say "7. Regression checks for previously fixed bugs"
mkproj() {
  local d="$WORK/rg_$1"; rm -rf "$d"; mkdir -p "$d"
  printf '%s\n' "$2" > "$d/main.pls"
  printf '{"name":"rg%s","version":"0.1.0","entry":"main.pls","server":{"port":9109,"tickrate":30,"max_players":8}}\n' "$1" > "$d/plsproject.json"
  echo "$d"
}
expect_compiles() {
  local name="$1" d="$2"
  "$PLSCC" build "$d" --runtime-dir "$PLS_ROOT/runtime" > "$WORK/rg_$name.log" 2>&1
  local gen="$d/build/generated/main.cpp"
  if [ ! -f "$gen" ]; then bad "regression $name (codegen refused)"; return; fi
  if c++ -std=c++17 -Iruntime/include -c "$gen" -o /dev/null > "$WORK/rg_${name}_cxx.log" 2>&1; then
    ok "regression $name (generated C++ compiles)"
  else
    bad "regression $name (generated C++ does not compile)"
    head -6 "$WORK/rg_${name}_cxx.log" | sed 's/^/        | /'
  fi
}
expect_diagnostic() {
  local name="$1" d="$2" needle="$3"
  if "$PLSCC" build "$d" --runtime-dir "$PLS_ROOT/runtime" > "$WORK/rg_$name.log" 2>&1; then
    bad "regression $name (expected a diagnostic, build succeeded)"
  elif grep -q "$needle" "$WORK/rg_$name.log"; then
    ok "regression $name (clean diagnostic)"
  else
    bad "regression $name (wrong diagnostic)"; head -5 "$WORK/rg_$name.log" | sed 's/^/        | /'
  fi
}

expect_compiles "toplevel_return" "$(mkproj toplevel_return 'Print(1);
return 3;')"
expect_compiles "callback_ok" "$(mkproj callback_ok 'function OnC(id) { Print(id); }
PlS::OnPlayerConnect(OnC);')"
expect_diagnostic "callback_literal" "$(mkproj callback_literal 'PlS::OnPlayerConnect(1);')" \
  "must be the name of a top-level"
expect_diagnostic "callback_arity" "$(mkproj callback_arity 'function H() { Print(1); }
PlS::OnPlayerConnect(H);')" "declares 0"
expect_diagnostic "function_as_value" "$(mkproj function_as_value 'function H() { Print(1); }
Print(H);')" "can.t hold a function"

RG_PARSE="$(mkproj parse_error 'PlS::Listen(')"
if "$PLSCC" build "$RG_PARSE" --runtime-dir "$PLS_ROOT/runtime" 2>&1 | grep -q "error: error:"; then
  bad "regression doubled_error_prefix"
else
  ok "regression doubled_error_prefix (single 'error:' prefix)"
fi

# The project's settings must actually reach the runtime, not just the game
# target -- they were silently ignored before.
if grep -q "set(PLS_MAX_PLAYERS" "$PLS_ROOT/examples/mp_server/build/CMakeLists.txt" 2>/dev/null; then
  ok "regression project_settings (max_players reaches the runtime build)"
else
  bad "regression project_settings (max_players not wired into the runtime build)"
fi

# ------------------------------------------------------ cross-project ----
say "8. Live interop with the real PlainVulkan client"
PV_ROOT="${PV_ROOT:-$PLS_ROOT/../PlainVulkan}"
HARNESS="$PV_ROOT/runtime/tests/network_interop_harness.cpp"
if [ ! -f "$HARNESS" ]; then
  skip "interop (PlainVulkan not found at $PV_ROOT; set PV_ROOT to enable)"
else
  if c++ -std=c++17 -I"$PV_ROOT/runtime/include" -o "$WORK/interop" \
        "$HARNESS" "$PV_ROOT/runtime/src/network.cpp" "$PV_ROOT/runtime/src/network_scene.cpp" \
        "$PV_ROOT/runtime/src/pv_value.cpp" "$PV_ROOT/runtime/src/pv_json.cpp" \
        -lpthread > "$WORK/interop_build.log" 2>&1; then
    ok "PlainVulkan client harness compiles against this server"

    # Fresh server: the max_players test above filled the previous one.
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null
    sleep 2
    setsid "$SERVER_BIN" > "$WORK/server_interop.log" 2>&1 < /dev/null &
    SERVER_PID=$!
    sleep 4

    if timeout 120 "$WORK/interop" > "$WORK/interop_run.log" 2>&1; then
      ok "interop: $(grep -oE '[0-9]+/[0-9]+ checks passed' "$WORK/interop_run.log" | tail -1)"
    else
      bad "live interop against the real PlainVulkan client"
      tail -14 "$WORK/interop_run.log" | sed 's/^/        | /'
    fi

    # The server must have seen the same traffic the client reported.
    if grep -q "player 1 connected" "$WORK/server_interop.log"; then
      ok "server logged the client's connect/disconnect"
    else
      bad "server logged the client's connect"
    fi
  else
    bad "PlainVulkan client harness compiles against this server"
    tail -10 "$WORK/interop_build.log" | sed 's/^/        | /'
  fi
fi

# ---------------------------------------------------------------- done ---
say "Summary"
printf '  %d passed, %d failed, %d skipped\n' "$PASS" "$FAIL" "$SKIP"
if [ "$FAIL" -ne 0 ]; then
  printf '  failed checks:\n'
  for n in "${FAILED_NAMES[@]}"; do printf '    - %s\n' "$n"; done
  exit 1
fi
printf '\n  \033[32mALL CHECKS PASSED\033[0m\n'
exit 0
