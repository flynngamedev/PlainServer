//! Every `PlS::` server command, paired with its expected argument count,
//! plus the small set of bare-name builtin functions.
//!
//! `codegen.rs` checks each call site against these tables before emitting
//! C++, so a wrong argument count is a precise `plscc` diagnostic instead of
//! a much less readable C++ error two build stages later.
//!
//! As in PlainVulkan, the list is written out rather than derived: the
//! grammar treats `Namespace::name(...)` fully generically (see
//! pls.lalrpop), so nothing forces this list and the C++ header to agree
//! syntactically. `scripts/check_command_coverage.sh` is what enforces it,
//! and should be run after adding a command.

pub struct CommandSig {
    pub name: &'static str,
    pub arity: usize,
}

macro_rules! sig {
    ($name:literal, $arity:literal) => {
        CommandSig { name: $name, arity: $arity }
    };
}

pub const PLS_COMMANDS: &[CommandSig] = &[
    // Server setup & lifecycle
    sig!("Listen", 1),
    sig!("SetTickrate", 1),
    sig!("Run", 0),
    sig!("Shutdown", 0),
    sig!("GetTick", 0),
    sig!("GetElapsedTime", 0),
    // Player & connection management
    sig!("OnPlayerConnect", 1),
    sig!("OnPlayerDisconnect", 1),
    sig!("OnPlayerInput", 1),
    sig!("GetConnectedPlayers", 0),
    sig!("KickPlayer", 2),
    sig!("IsPlayerConnected", 1),
    sig!("GetPlayerAddr", 1),
    sig!("GetPlayerLastSeen", 1),
    sig!("CheckTimeouts", 1),
    // Receiving input
    sig!("ReceiveInput", 0),
    sig!("ReceiveInputNonBlocking", 0),
    sig!("GetPlayerLastInput", 1),
    sig!("ParseInput", 2),
    sig!("ValidateInput", 1),
    sig!("SetMaxPlayerSpeed", 1),
    sig!("GetPlayerSessionToken", 1),
    // World state management
    sig!("CreateEntity", 1),
    sig!("DestroyEntity", 1),
    sig!("GetEntity", 1),
    sig!("GetAllEntities", 0),
    sig!("SetEntityPosition", 4),
    sig!("GetEntityPosition", 1),
    sig!("SetEntityRotation", 4),
    sig!("SetEntityVelocity", 4),
    sig!("SetEntityHealth", 2),
    sig!("GetEntityHealth", 1),
    sig!("SetEntityState", 2),
    sig!("SetEntityRadius", 2),
    // Physics & game logic
    sig!("UpdatePhysics", 1),
    sig!("CheckCollisions", 0),
    sig!("GetCollisionsForEntity", 1),
    sig!("ApplyForce", 4),
    sig!("RaycastFromTo", 6),
    sig!("Distance", 2),
    sig!("FindEntitiesInSphere", 4),
    // Broadcasting state
    sig!("BroadcastState", 0),
    sig!("BroadcastStateSelective", 0),
    sig!("SendToPlayer", 2),
    sig!("SendToAllPlayers", 1),
    sig!("SetBroadcastFormat", 1),
    sig!("SetVisibilityRange", 1),
    sig!("SetBroadcastInterval", 1),
    // Serialization helpers
    sig!("SerializeEntity", 1),
    sig!("SerializeAllEntities", 0),
    sig!("DeserializeInput", 1),
    sig!("GetEntityData", 1),
    // Debugging & monitoring
    sig!("Print", 1),
    sig!("PrintStats", 0),
    sig!("EnableDebugLogging", 1),
    sig!("GetServerStats", 0),
    sig!("RecordTick", 1),
    sig!("ReplayTick", 1),
];

/// Bare-name functions callable without a namespace: `Print("hi")` rather
/// than `PlS::Print("hi")`.
///
/// These exist because a server script is ordinary imperative code that
/// needs to measure a distance, take an absolute value, or count the
/// elements of an array -- none of which are *server* operations, so
/// putting them behind `PlS::` would misrepresent them, and none of which
/// PlainS has operators for.
///
/// A user-defined function always wins over a builtin of the same name (see
/// `Codegen::user_functions`): a script that defines its own `Print` gets
/// its own `Print`, rather than silently calling ours.
pub const BUILTINS: &[CommandSig] = &[
    sig!("Print", 1),   // also available as PlS::Print; the doc examples use both
    sig!("Len", 1),     // array/object/string length
    sig!("Keys", 1),    // object -> array of keys
    sig!("Has", 2),     // object, key -> bool
    sig!("Push", 2),    // array, value -> array (mutates in place)
    sig!("Str", 1),
    sig!("Int", 1),
    sig!("Float", 1),
    sig!("Abs", 1),
    sig!("Min", 2),
    sig!("Max", 2),
    sig!("Sqrt", 1),
    sig!("Floor", 1),
    sig!("Ceil", 1),
    sig!("Random", 0), // uniform in [0, 1)
];

pub fn lookup(name: &str) -> Option<&'static CommandSig> {
    PLS_COMMANDS.iter().find(|c| c.name == name)
}

pub fn lookup_builtin(name: &str) -> Option<&'static CommandSig> {
    BUILTINS.iter().find(|c| c.name == name)
}
