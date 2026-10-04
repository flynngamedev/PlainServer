// main.pls -- an authoritative multiplayer game server.
//
// Run it with:   python3 pls.py run examples/mp_server
//
// Two things happen automatically and are worth knowing about, because
// nothing in this file appears to call them:
//
//   * Main() is called at startup.
//   * Tick() is registered as the per-tick handler, and PlS::Run() calls it
//     once per server tick.
//
// Both are wired up by the compiler when it sees top-level functions with
// those names and no parameters. See compiler/src/codegen.rs.

var tickrate = 60;
var visibility_range = 100.0;
var player_timeout = 30.0;
var move_speed = 0.1;
var shoot_damage = 25;

function Main() {
    PlS::SetTickrate(tickrate);
    PlS::SetVisibilityRange(visibility_range);
    PlS::SetMaxPlayerSpeed(12.0);

    PlS::OnPlayerConnect(OnConnect);
    PlS::OnPlayerDisconnect(OnDisconnect);
    PlS::OnPlayerInput(OnInput);

    PlS::Listen(8080);
    PlS::Run();  // blocking -- runs until PlS::Shutdown()
}

// The player's entity already exists by the time this runs, created by the
// runtime under the SAME id as the player. That's why every command below
// passes `player_id` where an entity id is expected.
function OnConnect(player_id) {
    Print("Player " + player_id + " connected from " + PlS::GetPlayerAddr(player_id));

    PlS::SetEntityPosition(player_id, 0, 1, 0);
    PlS::SetEntityHealth(player_id, 100);
    PlS::SetEntityState(player_id, "alive");

    PlS::SendToPlayer(player_id, {
        "type": "welcome",
        "player_id": player_id,
        "initial_position": [0, 1, 0],
        "tickrate": tickrate
    });
}

function OnDisconnect(player_id, reason) {
    Print("Player " + player_id + " disconnected: " + reason);
    // The player's entity is destroyed by the runtime before this runs, so
    // there is nothing to clean up here beyond your own game state.
}

function OnInput(player_id, input) {
    // Every input came from a client and is therefore untrusted. This
    // rejects the malformed cases (missing action, NaN coordinates); the
    // game-specific limits are enforced by the handlers below.
    if (!PlS::ValidateInput(input)) {
        Print("Rejected malformed input from player " + player_id);
        return;
    }

    var action = input["action"];
    if (action == "move") {
        HandleMove(player_id, input["x"], input["y"], input["z"]);
    } elif (action == "shoot") {
        HandleShoot(player_id, input["x"], input["y"], input["z"]);
    } elif (action == "jump") {
        HandleJump(player_id);
    }
}

function HandleMove(player_id, x, y, z) {
    if (PlS::GetEntityHealth(player_id) <= 0) { return; }

    // The client sends a DIRECTION, never a position: the server decides
    // where that puts them. A client that sent its own position could
    // simply claim to be anywhere.
    var pos = PlS::GetEntityPosition(player_id);
    var nx = pos[0] + x * move_speed;
    var ny = pos[1] + y * move_speed;
    var nz = pos[2] + z * move_speed;
    PlS::SetEntityPosition(player_id, nx, ny, nz);
}

function HandleShoot(player_id, aim_x, aim_y, aim_z) {
    if (PlS::GetEntityHealth(player_id) <= 0) { return; }

    var pos = PlS::GetEntityPosition(player_id);
    var range = 100.0;
    var hit = PlS::RaycastFromTo(
        pos[0], pos[1], pos[2],
        pos[0] + aim_x * range, pos[1] + aim_y * range, pos[2] + aim_z * range);

    if (!hit["hit"]) { return; }

    var target = hit["entity_id"];
    if (target == player_id) { return; }  // the ray starts inside the shooter

    var health = PlS::GetEntityHealth(target) - shoot_damage;
    PlS::SetEntityHealth(target, health);

    if (health <= 0) {
        PlS::SetEntityState(target, "dead");
        PlS::SendToAllPlayers({
            "type": "player_killed",
            "killer": player_id,
            "victim": target
        });
    }
}

function HandleJump(player_id) {
    PlS::ApplyForce(player_id, 0, 10, 0);
}

// Called once per tick by PlS::Run().
function Tick() {
    PlS::UpdatePhysics(1.0 / tickrate);
    PlS::CheckCollisions();
    PlS::BroadcastStateSelective();
    PlS::CheckTimeouts(player_timeout);

    if (PlS::GetTick() % 300 == 0) {   // every five seconds at 60Hz
        PlS::PrintStats();
    }
}
