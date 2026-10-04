//! Parser tests for PlainS.
//!
//! The emphasis is on the three places PlainS's grammar deliberately
//! differs from PlainVulkan's -- object literals, for-in, and the removal
//! of the bare block statement -- since those are the parts with no
//! upstream test coverage to inherit, and the parts where an LALR(1)
//! conflict would show up.

use pls_transpiler::ast::*;
use pls_transpiler::{parse, parse_expr};

fn ok(src: &str) -> Program {
    match parse(src) {
        Ok(p) => p,
        Err(diags) => panic!("expected `{}` to parse, got: {:?}", src, diags),
    }
}

fn fails(src: &str) {
    if parse(src).is_ok() {
        panic!("expected `{}` to be rejected, but it parsed", src);
    }
}

// ---------------------------------------------------------------------
// Object literals
// ---------------------------------------------------------------------

#[test]
fn object_literal_with_string_keys() {
    let e = parse_expr(r#"{"type": "welcome", "id": 3}"#).unwrap();
    match e {
        Expr::ObjectLiteral(fields, _) => {
            assert_eq!(fields.len(), 2);
            assert_eq!(fields[0].0, "type");
            assert_eq!(fields[1].0, "id");
        }
        other => panic!("expected ObjectLiteral, got {:?}", other),
    }
}

#[test]
fn object_literal_with_bare_identifier_keys() {
    // `{type: "x"}` and `{"type": "x"}` must produce the same key.
    let e = parse_expr(r#"{type: "welcome"}"#).unwrap();
    match e {
        Expr::ObjectLiteral(fields, _) => assert_eq!(fields[0].0, "type"),
        other => panic!("expected ObjectLiteral, got {:?}", other),
    }
}

#[test]
fn object_literal_empty_and_trailing_comma() {
    parse_expr("{}").unwrap();
    parse_expr(r#"{"a": 1,}"#).unwrap();
}

#[test]
fn object_literal_nests_with_arrays_and_objects() {
    let src = r#"var packet = {
        "type": "state",
        "entities": { "1": { "position": [0, 1, 2] } },
        "tags": ["a", "b"]
    };"#;
    ok(src);
}

#[test]
fn object_literal_as_call_argument() {
    // The case that motivated the syntax in the first place.
    ok(r#"PlS::SendToPlayer(id, {"type": "welcome", "player_id": id});"#);
}

#[test]
fn object_literal_value_can_be_any_expression() {
    ok(r#"var o = {"sum": 1 + 2 * 3, "call": PlS::GetTick(), "tern": a ? b : c};"#);
}

#[test]
fn ternary_colon_is_not_confused_with_an_object_key() {
    // Both `?:` and object literals use `:`; this is the case that would
    // break if the grammar resolved that ambiguity the wrong way.
    let e = parse_expr("a ? b : c").unwrap();
    assert!(matches!(e, Expr::Ternary(..)));
    ok(r#"var x = {"k": a ? b : c};"#);
}

// ---------------------------------------------------------------------
// for-in
// ---------------------------------------------------------------------

#[test]
fn for_in_parses() {
    let p = ok("for (pid in players) { Print(pid); }");
    match &p.stmts[0] {
        Stmt::ForIn(f) => {
            assert_eq!(f.var_name, "pid");
            assert!(matches!(f.iterable, Expr::Ident(..)));
        }
        other => panic!("expected ForIn, got {:?}", other),
    }
}

#[test]
fn for_in_over_a_call_and_an_index() {
    ok("for (p in PlS::GetConnectedPlayers()) { Print(p); }");
    ok(r#"for (id in state["entities"]) { Print(id); }"#);
}

#[test]
fn counted_for_still_parses_alongside_for_in() {
    // Both begin `for ( ident`, so this is the pair that would collide if
    // one token of lookahead were not enough to tell them apart.
    let p = ok("for (i = 0; i < 3; i = i + 1) { Print(i); }");
    assert!(matches!(&p.stmts[0], Stmt::For(_)));

    let p = ok("for (var i = 0; i < 3; i = i + 1) { Print(i); }");
    assert!(matches!(&p.stmts[0], Stmt::For(_)));

    let p = ok("for (i in xs) { Print(i); }");
    assert!(matches!(&p.stmts[0], Stmt::ForIn(_)));
}

#[test]
fn for_in_nests() {
    ok("for (a in xs) { for (b in ys) { Print(a + b); } }");
}

#[test]
fn in_is_a_keyword_and_no_longer_an_identifier() {
    // Adding `in` to the keyword set is a breaking change for any script
    // that used it as a variable name; this pins the behaviour so the
    // change is visible rather than surprising.
    fails("var in = 1;");
}

// ---------------------------------------------------------------------
// No bare block statement (the trade that pays for object literals)
// ---------------------------------------------------------------------

#[test]
fn bare_block_statement_is_rejected() {
    // `{ Print(1); }` at statement position is no longer a block. It is
    // read as an object literal and fails, which is the intended and
    // documented consequence.
    fails("{ Print(1); }");
}

#[test]
fn blocks_still_work_where_control_flow_requires_them() {
    ok("if (a) { Print(1); } elif (b) { Print(2); } else { Print(3); }");
    ok("while (a) { Print(1); }");
    ok("function f() { Print(1); }");
    ok("for (x in xs) { Print(x); }");
}

// ---------------------------------------------------------------------
// Shared language surface (inherited from PlainVulkan's grammar)
// ---------------------------------------------------------------------

#[test]
fn declarations_and_assignment_forms() {
    ok("var a = 1; let b = 2; const c = 3;");
    ok("a = 1; a += 2; a -= 3; a *= 4; a /= 5; a %= 6;");
    ok("a[0] = 1; a.b = 2; a.b[0].c = 3;");
}

#[test]
fn invalid_assignment_target_is_rejected() {
    fails("1 + 2 = 3;");
    fails("PlS::GetTick() = 5;");
}

#[test]
fn namespaced_calls_parse_generically() {
    let e = parse_expr("PlS::SetEntityPosition(id, 1, 2, 3)").unwrap();
    let (ns, name, args) = e.as_scoped_call().expect("expected a scoped call");
    assert_eq!(ns, "PlS");
    assert_eq!(name, "SetEntityPosition");
    assert_eq!(args.len(), 4);
}

#[test]
fn comments_are_stripped() {
    ok("// line comment\nvar a = 1; /* block\ncomment */ var b = 2;");
}

#[test]
fn operator_precedence_binds_as_expected() {
    // 1 + 2 * 3 must be 1 + (2 * 3), not (1 + 2) * 3.
    let e = parse_expr("1 + 2 * 3").unwrap();
    match e {
        Expr::Binary(BinaryOp::Add, _, rhs, _) => {
            assert!(matches!(*rhs, Expr::Binary(BinaryOp::Mul, ..)));
        }
        other => panic!("expected Add at the root, got {:?}", other),
    }
}

#[test]
fn the_reference_example_server_parses() {
    // A condensed version of examples/mp_server/main.pls, exercising the
    // new syntax and the shared syntax together in one program.
    let src = r#"
var tickrate = 60;

function Main() {
    PlS::SetTickrate(tickrate);
    PlS::OnPlayerConnect(OnConnect);
    PlS::Listen(8080);
    PlS::Run();
}

function OnConnect(player_id) {
    Print("Player " + player_id + " connected");
    PlS::SendToPlayer(player_id, {
        "type": "welcome",
        "player_id": player_id,
        "initial_position": [0, 1, 0]
    });
}

function OnInput(player_id, input) {
    if (!PlS::ValidateInput(input)) { return; }
    var action = input["action"];
    if (action == "move") {
        var pos = PlS::GetEntityPosition(player_id);
        PlS::SetEntityPosition(player_id, pos[0] + input["x"], pos[1], pos[2]);
    } elif (action == "jump") {
        PlS::ApplyForce(player_id, 0, 10, 0);
    }
}

function Tick() {
    PlS::UpdatePhysics(1.0 / tickrate);
    for (pid in PlS::GetConnectedPlayers()) {
        if (PlS::GetPlayerLastSeen(pid) > 30) {
            PlS::KickPlayer(pid, "timeout");
        }
    }
    PlS::BroadcastStateSelective();
}
"#;
    let p = ok(src);
    assert_eq!(p.stmts.len(), 5); // one var + four functions
}
