# PlainS runtime -- what's real, what's simplified, what's stubbed

The runtime backs all 56 `PlS::` commands with real C++ symbols
(cross-checked against `compiler/src/pls_commands.rs` by
`../scripts/check_command_coverage.sh` -- every declared function has
exactly one definition, nothing is missing).

"Backed by a real symbol" is not the same as "fully implemented," so this
file is the honest map of which is which, using the same tiers as
PlainVulkan's `runtime/README.md`:

- **Tier 1** -- genuinely complete; no known gaps.
- **Tier 2** -- fully functional, straightforward mapping onto a real
  facility (BSD sockets, Winsock).
- **Tier 3** -- really works, with a deliberate scope simplification
  documented inline.
- **Tier 4** -- the API is real and safe to call (never crashes), but the
  command doesn't yet have its real effect. Logs a warning.

**This runtime has been built and run**, not merely compiled. See
"Verification" at the bottom.

## Server lifecycle & connections (Tier 1-2)

`server.cpp`. Real TCP: `socket`/`bind`/`listen`/`accept` with
`SO_REUSEADDR`, non-blocking sockets, `TCP_NODELAY`, and `select()` in the
gap between ticks. Winsock on Windows, BSD sockets elsewhere.

`PlS::Run()` holds a fixed timestep: it computes the next tick deadline,
services the network with exactly the remaining budget, ticks on schedule,
and resynchronizes (with a warning under `EnableDebugLogging`) if a tick
overruns. Measured at 60Hz over 60 seconds it landed on tick 3600 at 60.01s.

Connection handling is deferred throughout -- reads mark connections for
close, and a reaping pass afterwards fires `OnPlayerDisconnect` and destroys
entities. This is what lets a handler kick a player or destroy an entity
without invalidating an iterator the read loop is still holding.

`PlS::CheckTimeouts` is not in the original command list, but the reference's
own example server calls it, and every server needs it: a client that loses
power never closes its socket and would otherwise hold a slot until the OS
keepalive expires (hours, by default).

## Input (Tier 1-3)

`PlS::ReceiveInput()` "blocks" by pumping the network until something
arrives -- on a single-threaded server it could not sleep, because this
thread is the only thing that reads sockets.

`PlS::ValidateInput` (Tier 3) is a cheap universal check: the value is an
object, `action` is a non-empty string, and any `x`/`y`/`z` present are
finite numbers. It rejects the inputs that would corrupt the *simulation*
(a NaN silently poisoning every position it touches, and which cannot even
be represented in JSON). It is **not** game-specific validation -- it cannot
know that a move of 400 units in one tick is impossible in your game. Do
that in your handler.

`PlS::ParseInput(data, "json")` is real. **`"msgpack"` and `"binary"` are
Tier 4**: they log a warning and return the value unparsed.

## World state (Tier 1)

`world.cpp`. Entities live in a `std::map` keyed by id, and **ids are never
reused** -- a stale id from a client resolves to "gone", not to whichever
entity now occupies that slot, which avoids a whole class of exploit for the
cost of a pointer chase.

A player's entity is created on connect under the *same id as the player*.
See the note in `include/pls/pls_server.h`.

## Physics (Tier 3)

`physics.cpp`. **Every entity is a sphere with one radius.** That is not a
placeholder for "real" collision shapes; it is the right fidelity for this
layer. An authoritative server does not need to reproduce the client's
rendering geometry -- it needs to answer "could this player plausibly have
moved there / hit that" cheaply and identically for every client. Sphere
tests are exact, branch-free, order-independent, and cost one dot product.
Convex hulls or triangle meshes would multiply the per-tick cost by orders
of magnitude and introduce solver tolerances that make a tick
non-reproducible -- which would in turn make `RecordTick`/`ReplayTick`
useless for the debugging they exist for.

Games needing precise hit geometry typically layer their own check on top of
these answers, which is why `PlS::CheckCollisions` returns pairs rather than
resolving them.

Other deliberate choices:

- `UpdatePhysics` clamps `dt` to 0.25s. A server paused in a debugger, or
  descheduled by a loaded host, would otherwise resume with a huge step and
  teleport everything through the world.
- `ApplyForce` is an impulse straight to velocity: entities have no mass,
  because a server arbitrating movement rather than simulating rigid bodies
  has no use for one. "Force" is the reference's name, so it is kept.
- `CheckCollisions` is O(n²) over pairs. Honest about its limit: fine to a
  few hundred entities, and the point where a spatial hash would earn its
  complexity is well past what this runtime targets.
- `RaycastFromTo` keeps the **nearest** hit, which is what makes a shot stop
  at the wall in front of the target rather than passing through it.

## Broadcasting (Tier 1-2, one Tier 4)

`broadcast.cpp`. `BroadcastState` serializes once and sends identical bytes
to everyone. `BroadcastStateSelective` encodes per player, which is more
server CPU in exchange for less bandwidth per client and -- more importantly
-- not handing every client the positions of players they cannot see, which
is the data a wallhack is built from.

**`PlS::SetBroadcastFormat` is Tier 4 for `"msgpack"` and `"binary"`.** Both
are accepted and warn once, then JSON is sent anyway. Falling back rather
than refusing is the safer failure: a script that set a format it doesn't
truly depend on keeps working, and the client -- which only speaks JSON
frames -- can still read what arrives.

Non-finite floats are written as `0` rather than emitted as `NaN`, which
JSON cannot represent; a packet containing one would be rejected wholesale
by the client's parser, disconnecting every player at once over what is
already a physics bug.

## Debugging (Tier 1-3)

`debug.cpp`. `RecordTick` snapshots the world into a **ring buffer** (256
ticks) -- an unbounded log would consume memory proportional to uptime and
eventually become the bug you were chasing.

`ReplayTick` **returns** the snapshot rather than restoring it (Tier 3).
Restoring would silently diverge the server from every connected client,
whose state cannot be rewound; handing the snapshot back is what the
"step-through debugging" use actually needs.

## Value & JSON layer (Tier 1)

`pls_value.cpp` / `pls_json.cpp` are the same implementations PlainVulkan
uses, in the `pls` namespace. Keeping them interchangeable is what lets both
ends of a connection share one Value↔JSON conversion, so a value means the
same thing on both sides.

Extensions over the client's version: `make_object` (for object literals),
and `iter_count`/`iter_at` backing the `for (x in y)` loop. Arrays iterate
elements, objects iterate **keys**, strings iterate characters, and anything
else iterates zero times rather than erroring -- a loop over a null result
should do nothing, which is what a script means by it.

## Verification

Built with CMake + g++ and run:

- **11/11** protocol checks from a raw-socket client: welcome packets,
  server-authoritative movement (client sends a direction, server computes
  the position), raycast damage applied to the entity actually in the line
  of fire, misses damaging nobody, malformed input rejected without dropping
  the connection, and disconnect removing the player's entity.
- **13/13** interop checks against PlainVulkan's *actual* client networking
  code, linked unmodified against a stub scene layer: connect, callbacks,
  input round trip (position mirrored back exactly), PING/PONG latency
  (0.044ms on loopback), byte counters, and clean disconnect.
- All sources compile clean under `-Wall -Wextra`.
- `scripts/check_command_coverage.sh`: all 56 commands consistent.
- `cargo test`: 27 tests (7 lexer, 20 parser).

## If you want to extend something here

Adding a command means touching four places, and the coverage script exists
because nothing else forces them to agree:

1. `include/pls/pls_runtime.h` -- declare it.
2. one of the command sources (`server`/`world`/`physics`/`broadcast`/
   `debug`.cpp) -- define it exactly once, at column 0. File-local helpers
   in those files are marked `static` by convention so they are not mistaken
   for commands.
3. `../compiler/src/pls_commands.rs` -- add its arity.
4. `../scripts/check_command_coverage.sh` -- run it.

A new command source file must also be added to `COMMAND_SOURCES` in that
script and to `CMakeLists.txt`.
