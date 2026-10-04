#!/usr/bin/env bash
#
# verify_msvc.sh -- build and test PlainS with the real Microsoft C++
# compiler, from Linux, using msvc-wine.
#
# This is not a MinGW cross-compile and not a Wine-only approximation of a
# compiler: cl.exe and link.exe are Microsoft's own binaries, running under
# Wine, producing PE objects with the Microsoft CRT and the Windows SDK.
# The compiler is exactly what a `Developer Command Prompt` on Windows would
# invoke. What Wine substitutes for is the *operating system* underneath,
# not the toolchain.
#
# ---------------------------------------------------------------------
# One-time setup
# ---------------------------------------------------------------------
#   sudo apt-get install -y wine64 msitools winbind cmake ninja-build
#   git clone https://github.com/mstorsjo/msvc-wine
#   cd msvc-wine
#   ./vsdownload.py --accept-license --dest /opt/msvc \
#       --architecture x64 --host-arch x64 --only-host yes \
#       --with-default no --with-workload no --with-atl no --with-dia no \
#       --with-msbuild no --with-devcmd no --with-asan no \
#       --with-msvc yes --with-sdk yes
#   ./install.sh /opt/msvc
#
# The narrow --with-* selection matters: the default set pulls in the whole
# IDE payload (test tools, vcpkg, ATL, CodeMap) and takes an order of
# magnitude longer for packages this build never touches.
#
# Downloading MSVC this way is subject to the Visual Studio licence you
# accept with --accept-license. Read it; it governs what you may do with
# the resulting toolchain.
#
# Usage:  bash scripts/verify_msvc.sh
#         MSVC_ROOT=/opt/msvc bash scripts/verify_msvc.sh
#
set -uo pipefail

MSVC_ROOT=${MSVC_ROOT:-/opt/msvc}
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK=${WORK:-/tmp/plains_msvc}

RED=$'\033[31m'; GREEN=$'\033[32m'; BOLD=$'\033[1m'; OFF=$'\033[0m'
PASSED=0; FAILED=0; FAILED_NAMES=()

pass() { PASSED=$((PASSED+1)); printf '  %sPASS%s  %s\n' "$GREEN" "$OFF" "$1"; }
fail() { FAILED=$((FAILED+1)); FAILED_NAMES+=("$1"); printf '  %sFAIL%s  %s\n' "$RED" "$OFF" "$1"
         [ -n "${2:-}" ] && sed 's/^/        | /' <<< "$2"; }
section() { printf '\n%s== %s ==%s\n' "$BOLD" "$1" "$OFF"; }

if [ ! -x "$MSVC_ROOT/bin/x64/cl" ]; then
  echo "msvc-wine not found at $MSVC_ROOT (expected $MSVC_ROOT/bin/x64/cl)."
  echo "See the setup notes at the top of this script."
  exit 2
fi

export PATH="$MSVC_ROOT/bin/x64:$PATH"
export WINEDEBUG=${WINEDEBUG:--all}
mkdir -p "$WORK"

section "0. Toolchain identity"
CLVER=$(cl 2>&1 | grep -o 'Version [0-9.]*' | head -1)
if [ -n "$CLVER" ]; then pass "cl.exe is Microsoft's ($CLVER)"; else fail "cl.exe did not report a version"; fi

section "1. mp_server -- configure and build with MSVC"
rm -rf "$WORK/srv" && mkdir -p "$WORK/srv"
if cmake -S "$ROOT/examples/mp_server/build" -B "$WORK/srv" -G Ninja \
     -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=AMD64 \
     -DCMAKE_CXX_COMPILER=cl -DCMAKE_RC_COMPILER=rc \
     -DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded \
     -DCMAKE_POLICY_DEFAULT_CMP0141=NEW \
     -DCMAKE_BUILD_TYPE=Release > "$WORK/srv_cfg.log" 2>&1; then
  pass "cmake configure (MSVC detected by CMake)"
else
  fail "cmake configure" "$(tail -15 "$WORK/srv_cfg.log")"
fi
if ( cd "$WORK/srv" && ninja ) > "$WORK/srv_build.log" 2>&1; then
  pass "build mp_server.exe"
else
  fail "build mp_server.exe" "$(grep -E 'error C|fatal error|LNK[0-9]{4}' "$WORK/srv_build.log" | head -10)"
fi
WARNS=$(grep -c 'warning C' "$WORK/srv_build.log" || true)
if [ "$WARNS" -eq 0 ]; then pass "no MSVC warnings"; else fail "MSVC emitted $WARNS warning(s)" \
  "$(grep 'warning C' "$WORK/srv_build.log" | head -8)"; fi

section "2. runtime_tests -- compile with MSVC, run under Wine"
mkdir -p "$WORK/tests" && cd "$WORK/tests"
if cl /nologo /std:c++17 /EHsc /permissive- /Zc:__cplusplus /utf-8 /O2 /W3 \
     /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS \
     /I"$ROOT/runtime/include" \
     "$ROOT/runtime/tests/runtime_tests.cpp" "$ROOT/runtime/src/pls_value.cpp" \
     "$ROOT/runtime/src/pls_json.cpp" "$ROOT/runtime/src/net.cpp" \
     /Fe:runtime_tests.exe /link ws2_32.lib > "$WORK/tests_build.log" 2>&1; then
  pass "runtime_tests compile"
  OUT=$(wine ./runtime_tests.exe 2>&1)
  if grep -q '^PASS' <<< "$OUT"; then
    pass "runtime_tests pass ($(grep -o '[0-9]* checks' <<< "$OUT" | head -1))"
  else
    fail "runtime_tests" "$(tail -12 <<< "$OUT")"
  fi
else
  fail "runtime_tests compile" "$(grep -E 'error C' "$WORK/tests_build.log" | head -10)"
fi

section "3. The MSVC build actually serves clients"
BIN="$ROOT/examples/mp_server/build/bin/mp_server.exe"
if [ -f "$BIN" ]; then
  ( cd "$(dirname "$BIN")" && setsid nohup wine ./mp_server.exe > "$WORK/run.log" 2>&1 < /dev/null & echo $! > "$WORK/srv.pid" )
  sleep 8
  if grep -q "listening on port" "$WORK/run.log"; then pass "server binds its port"; else
    fail "server binds its port" "$(tail -8 "$WORK/run.log")"; fi
  if python3 - <<'PY'
import socket, sys
s = socket.socket(); s.settimeout(5)
try:
    s.connect(("127.0.0.1", 8080)); s.close()
except Exception as e:
    print(e); sys.exit(1)
PY
  then pass "accepts a TCP connection"; else fail "accepts a TCP connection"; fi
  sleep 4
  if grep -qE 'tick=[1-9][0-9]*' "$WORK/run.log"; then pass "tick loop advances"; else
    fail "tick loop advances" "$(tail -6 "$WORK/run.log")"; fi
  kill -9 "$(cat "$WORK/srv.pid")" 2>/dev/null
  wineserver -k 2>/dev/null
  sleep 2
else
  fail "mp_server.exe was not produced"
fi

section "Summary"
printf '  %d passed, %d failed\n' "$PASSED" "$FAILED"
if [ "$FAILED" -gt 0 ]; then
  printf '  failed checks:\n'; for n in "${FAILED_NAMES[@]}"; do printf '    - %s\n' "$n"; done
  printf '\n  %sMSVC VERIFICATION FAILED%s\n' "$RED" "$OFF"; exit 1
fi
printf '\n  %sALL MSVC CHECKS PASSED%s\n' "$GREEN" "$OFF"
