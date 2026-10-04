#!/usr/bin/env python3
"""
pls -- command-line tool for PlainServer (.pls) server projects.

Usage:
    pls new <name>          Create a new server project folder
    pls build <name>        Parse, generate C++, and compile a native binary
    pls run <name>          Build (if needed) and run the server
    pls check <file.pls>    Parse and compile-check a single file
    pls emit <file.pls>     Print the generated C++ to stdout
    pls doctor              Check that the native build dependencies are present
    pls version             Print the PlainServer toolchain version

This script is deliberately the *only* thing a PlainServer install asks you to
put on PATH. Everything beyond `pls new` (pure Python, no compilation) is
orchestration: it makes sure the Rust pieces (transpiler/, compiler/) are
built in release mode, then shells out to them, and finally -- for
build/run -- to CMake for the native C++ compile.

Note how much shorter `pls doctor` is than PlainVulkan's `pv doctor`: a
server needs a C++ compiler and CMake, and nothing else. No Vulkan SDK, no
GPU, no display. That is the whole point of keeping the server runtime free
of the renderer -- it has to build and run on a headless box.
"""

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

VERSION = "2.6"

# The directory this script lives in is the PlainServer install root --
# transpiler/, compiler/, and runtime/ are always siblings of pls.py itself,
# regardless of where the user copied `pls` on their PATH.
PLS_HOME = Path(__file__).resolve().parent
TRANSPILER_DIR = PLS_HOME / "transpiler"
COMPILER_DIR = PLS_HOME / "compiler"
RUNTIME_DIR = PLS_HOME / "runtime"


def usage(exit_code=2):
    print(__doc__.strip())
    sys.exit(exit_code)


def die(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(1)


def run(cmd, **kwargs):
    """Run a subprocess, streaming its output, and return the CompletedProcess."""
    print(f"$ {' '.join(str(c) for c in cmd)}")
    return subprocess.run(cmd, **kwargs)


def rustc_host():
    """Host triple rustc was built for, e.g. x86_64-pc-windows-gnu."""
    try:
        out = subprocess.check_output(["rustc", "-vV"], text=True, stderr=subprocess.STDOUT)
    except (OSError, subprocess.CalledProcessError):
        return ""
    for line in out.splitlines():
        if line.startswith("host:"):
            return line.split(":", 1)[1].strip()
    return ""


def windows_msvc_rustup_toolchain():
    """When rustc is the GNU Windows host, proc-macros still compile for
    GNU -- `--target msvc` is not enough, and windows-sys still needs
    MinGW dlltool. Switching the cargo *toolchain* to MSVC fixes host
    builds too. Returns None when no override is needed."""
    if os.name != "nt":
        return None
    host = rustc_host()
    if "windows-gnu" not in host:
        return None
    # Keep the same CPU as the gnu host (x86_64 or aarch64).
    cpu = host.split("-")[0]
    return f"stable-{cpu}-pc-windows-msvc"


def cargo_release_binary(crate_dir: Path, bin_name: str) -> Path:
    """Ensure `bin_name` is built in release mode, and return its path.

    Built on demand rather than assumed present, so a fresh clone works with
    no separate setup step. Cargo itself decides whether a rebuild is
    actually needed, so the common case costs one fast no-op invocation.
    """
    if shutil.which("cargo") is None:
        die("cargo not found on PATH. Install Rust from https://rustup.rs and try again.")

    exe = bin_name + (".exe" if os.name == "nt" else "")
    cargo = ["cargo"]
    toolchain = windows_msvc_rustup_toolchain()
    if toolchain:
        print(
            f"[pls] rustc host is GNU Windows, which needs MinGW dlltool.exe.\n"
            f"[pls] Using toolchain {toolchain} instead (same ABI as cl.exe)."
        )
        if not shutil.which("rustup"):
            die(
                "rustup is required to install the MSVC Rust toolchain.\n"
                "  rustup toolchain install " + toolchain + "\n"
                "  rustup default " + toolchain
            )
        add = run(["rustup", "toolchain", "install", toolchain, "--profile", "minimal"])
        if add.returncode != 0:
            die(
                f"could not install {toolchain}. From an x64 Native Tools prompt:\n"
                f"  rustup toolchain install {toolchain}\n"
                f"  rustup default {toolchain}"
            )
        cargo = ["cargo", f"+{toolchain}"]

    # GNU-host + MSVC toolchain still writes to target/release/ (host change,
    # not a cross --target).
    binary = PLS_HOME / "target" / "release" / exe
    result = run(cargo + ["build", "--release", "--bin", bin_name], cwd=PLS_HOME)
    if result.returncode != 0:
        die(f"failed to build {bin_name}")
    if not binary.exists():
        die(f"{bin_name} was built but not found at {binary}")
    return binary


TEMPLATE = '''// {name} -- a PlainServer game server.
//
// Main() is called at startup and Tick() runs once per server tick; both
// are wired up automatically by the compiler when it sees top-level
// functions with those names.

var tickrate = {tickrate};

function Main() {{
    PlS::SetTickrate(tickrate);
    PlS::OnPlayerConnect(OnConnect);
    PlS::OnPlayerDisconnect(OnDisconnect);
    PlS::OnPlayerInput(OnInput);
    PlS::Listen({port});
    PlS::Run();
}}

// The player's entity already exists here, under the same id as the player.
function OnConnect(player_id) {{
    Print("Player " + player_id + " joined");
    PlS::SetEntityPosition(player_id, 0, 1, 0);
    PlS::SendToPlayer(player_id, {{ "type": "welcome", "player_id": player_id }});
}}

function OnDisconnect(player_id, reason) {{
    Print("Player " + player_id + " left: " + reason);
}}

function OnInput(player_id, input) {{
    if (!PlS::ValidateInput(input)) {{ return; }}
    if (input["action"] == "move") {{
        var pos = PlS::GetEntityPosition(player_id);
        PlS::SetEntityPosition(player_id,
            pos[0] + input["x"] * 0.1,
            pos[1] + input["y"] * 0.1,
            pos[2] + input["z"] * 0.1);
    }}
}}

function Tick() {{
    PlS::UpdatePhysics(1.0 / tickrate);
    PlS::BroadcastStateSelective();
    PlS::CheckTimeouts(30);
}}
'''


def cmd_new(args):
    if not args:
        usage()
    name = args[0]
    project = Path(name).resolve()
    if project.exists():
        die(f"'{project}' already exists")
    project.mkdir(parents=True)

    port, tickrate = 8080, 60
    (project / "plsproject.json").write_text(
        json.dumps(
            {
                "name": project.name,
                "version": "0.1.0",
                "entry": "main.pls",
                "server": {"port": port, "tickrate": tickrate, "max_players": 64},
            },
            indent=2,
        )
        + "\n"
    )
    (project / "main.pls").write_text(
        TEMPLATE.format(name=project.name, port=port, tickrate=tickrate)
    )
    print(f"created {project}")
    print(f"  cd {project.name} && pls run .")


def cmd_check(args):
    if not args:
        usage()
    plscc = cargo_release_binary(COMPILER_DIR, "plscc")
    sys.exit(run([plscc, "check", args[0]]).returncode)


def cmd_emit(args):
    if not args:
        usage()
    plscc = cargo_release_binary(COMPILER_DIR, "plscc")
    sys.exit(run([plscc, "emit", args[0]]).returncode)


def cmd_build(args, run_after=False):
    if not args:
        usage()
    project = Path(args[0]).resolve()
    if not (project / "plsproject.json").exists():
        die(f"no plsproject.json in '{project}' -- is that a PlainServer project?")

    if shutil.which("cmake") is None:
        die("cmake not found on PATH. Install CMake 3.16 or newer.")

    plscc = cargo_release_binary(COMPILER_DIR, "plscc")
    cmd = [plscc, "run" if run_after else "build", str(project), "--runtime-dir", str(RUNTIME_DIR)]
    if "--release" in args:
        cmd.append("--release")
    # Anything after a bare `--` is forwarded to the server process itself.
    if "--" in args:
        cmd.append("--")
        cmd.extend(args[args.index("--") + 1:])
    sys.exit(run(cmd).returncode)


def cmd_doctor(_args):
    print(f"PlainServer {VERSION}")
    print(f"install root: {PLS_HOME}\n")

    problems = 0

    def check(name, found, hint):
        nonlocal problems
        if found:
            print(f"  ok    {name}: {found}")
        else:
            print(f"  MISSING {name}")
            print(f"          {hint}")
            problems += 1

    check("cargo", shutil.which("cargo"), "Install Rust from https://rustup.rs")
    if os.name == "nt":
        host = rustc_host()
        if "windows-gnu" in host and shutil.which("dlltool") is None:
            print(f"  note  rustc host is {host} (needs MinGW dlltool.exe)")
            print("        pls.py will use stable-*-pc-windows-msvc for cargo builds.")
            print("        To make that the default: rustup default stable-x86_64-pc-windows-msvc")
    check("cmake", shutil.which("cmake"), "Install CMake 3.16+ (https://cmake.org/download/)")

    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if os.name == "nt" and not compiler:
        compiler = shutil.which("cl")
    check(
        "C++ compiler",
        compiler,
        "Install g++/clang++ (Linux/macOS) or Visual Studio Build Tools (Windows)",
    )

    for d, label in ((TRANSPILER_DIR, "transpiler/"), (COMPILER_DIR, "compiler/"),
                     (RUNTIME_DIR, "runtime/")):
        check(label, str(d) if d.is_dir() else None,
              f"expected next to pls.py at {d} -- did you copy pls.py out of the install folder?")

    print()
    if problems:
        print(f"{problems} problem(s) found.")
        sys.exit(1)
    print("All dependencies present. Note that unlike PlainVulkan, no Vulkan SDK,")
    print("GPU, or display is needed -- a PlainServer process builds and runs headless.")


def cmd_version(_args):
    print(f"PlainServer {VERSION}")


def main():
    if len(sys.argv) < 2:
        usage()
    command, args = sys.argv[1], sys.argv[2:]
    handlers = {
        "new": cmd_new,
        "check": cmd_check,
        "emit": cmd_emit,
        "build": lambda a: cmd_build(a, run_after=False),
        "run": lambda a: cmd_build(a, run_after=True),
        "doctor": cmd_doctor,
        "version": cmd_version,
        "--version": cmd_version,
        "-v": cmd_version,
        "help": lambda _a: usage(0),
        "--help": lambda _a: usage(0),
        "-h": lambda _a: usage(0),
    }
    handler = handlers.get(command)
    if handler is None:
        print(f"error: unknown command '{command}'\n", file=sys.stderr)
        usage()
    handler(args)


if __name__ == "__main__":
    main()
