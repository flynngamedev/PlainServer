# PlainServer 2.6

Authoritative game servers for **PlainVulkan 3.7**. You write a `.pls`
script; `pls.py` transpiles it to C++ and links a headless runtime that
owns sockets, world state, and the tick loop.

Same lexer / LALRPOP / Rust-codegen shape as PlainVulkan. Same TCP wire
format, so a PlainVulkan client and a PlainServer process interoperate
with no extra protocol layer. The CLI command is still `pls`.

```
main.pls  →  transpiler  →  AST  →  compiler  →  C++  →  CMake + runtime/
```

## Requirements

A server needs **Python 3**, **Rust (`cargo`)**, **CMake**, and a **C++17
compiler**. No Vulkan SDK, GPU, or display.

```bash
python pls.py doctor
python pls.py version    # PlainServer 2.6
```

**Windows:** build from an **x64 Native Tools Command Prompt**. If
`cargo` is the GNU Windows toolchain, `pls.py` prefers
`stable-x86_64-pc-windows-msvc` so proc-macros do not need MinGW
`dlltool`. To make MSVC the default:

```
rustup toolchain install stable-x86_64-pc-windows-msvc
rustup default stable-x86_64-pc-windows-msvc
```

## Quick start

```bash
python pls.py new mygame
python pls.py run mygame
```

Example authoritative game:

```bash
python pls.py run examples/mp_server
# then, from the PlainVulkan 3.7 tree:
python pv.py run examples/multiplayer_client
```

## CLI

```
pls new <name>          Create a server project
pls build <dir>         Parse → generate C++ → compile
pls run <dir>           Build, then run
pls check <file.pls>    Parse and compile-check one file
pls emit <file.pls>     Print generated C++
pls doctor              Check build dependencies
pls version
```

## Language

`.pls` is `.pv` with three differences (see `transpiler/src/pls.lalrpop`):

| | `.pv` (client) | `.pls` (server) |
|---|---|---|
| Object literals `{"k": v}` | no | **yes** |
| `for (x in xs)` | no | **yes** |
| Bare block `{ ... }` as a statement | yes | **no** |

Bare blocks were dropped so `{` at the start of a statement is not
ambiguous with object literals under LALR(1). Use a nested `function` to
scope a temporary.

Everything else (`var` / `let` / `const`, `if` / `elif` / `else`,
`while`, counted `for`, functions, comments) matches `.pv`.

### `Main()` and `Tick()`

```pls
function Main() { ... }   // startup
function Tick() { ... }   // once per server tick; PlS::Run() calls it
```

If those top-level, no-parameter functions exist, the compiler registers
them. `PlS::Run()` blocks, so per-tick work has to be registered first.

### Builtins (no namespace)

`Print` `Len` `Keys` `Has` `Push` `Str` `Int` `Float` `Abs` `Min` `Max`
`Sqrt` `Floor` `Ceil` `Random`. A user function of the same name always
wins.

## `PlS::` commands

About 73 commands for lifecycle, connections, input, world state,
physics, broadcast, serialization, and debug — declared in
`runtime/include/pls/pls_runtime.h`. Notes: `runtime/README.md`.

`scripts/check_command_coverage.sh` keeps the header, C++ definitions,
and `compiler/src/pls_commands.rs` in agreement.

## Server rules worth knowing

**A player’s entity id equals their player id.** On connect, the runtime
allocates one id and creates the entity before `OnPlayerConnect`.

```pls
function OnConnect(player_id) {
    PlS::SetEntityPosition(player_id, 0, 1, 0);
}
```

**Callbacks must be top-level functions.** They become C++ function
pointers. Nested functions compile to capturing lambdas and will not
link as callbacks.

## Threading

One thread. Each tick: poll sockets, dispatch queued callbacks, call
`Tick()`, sleep the rest of the period. A slow `Tick()` delays packet
processing. `PlS::EnableDebugLogging(true)` reports tick overruns.

The client uses a background network thread so rendering is not stalled
on sockets. The server *is* the simulation, so inputs apply at a defined
point in the tick.

## Wire format (must match PlainVulkan)

Defined in `runtime/include/pls/pls_net.h` and PlainVulkan’s
`runtime/include/pv/pv_net.h`:

```
[0..4)  uint32 big-endian: length of the rest
[4]     uint8 frame kind — 0 JSON, 1 raw bytes, 2 ping, 3 pong
[5..)   payload
```

Change both sides together. State packets look like:

```json
{ "type": "state", "tick": 1240,
  "entities": { "3": { "id": 3, "name": "Player_3", "state": "alive",
                       "health": 100, "position": [x,y,z],
                       "rotation": [p,y,r], "velocity": [vx,vy,vz] } } }
```

`entities` is keyed by string id so a selective broadcast can send a
different subset to each player.

## Layout

```
pls.py                     CLI
transpiler/                lexer, LALRPOP, AST → plsc
compiler/                  codegen, arity tables → plscc
runtime/                   C++ server runtime (static lib)
examples/mp_server/        sample authoritative server
scripts/                   check_command_coverage.sh
```
