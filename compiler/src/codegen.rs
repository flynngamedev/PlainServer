//! AST -> C++ code generation for PlainS.
//!
//! Shares PlainVulkan's core strategy, for the same reasons documented in
//! its codegen.rs: no type annotations anywhere in the grammar, so every
//! variable/parameter/return value compiles to the single dynamic
//! `pls::Value`; every user identifier gets a `u_` prefix so a script name
//! can never collide with a C++ keyword, a stdlib name, or one of this
//! file's generated helpers; and assignment targets go through
//! `pls::index_ref` / `pls::member_ref`, which auto-vivify and return
//! `Value&` so `a.b[0].c = v` becomes one chained C++ expression.
//!
//! What is specific to the server language:
//!
//! * `PlS::Foo(args)` -> `pls::rt::Foo(args)`, arity-checked first.
//!
//! * BARE BUILTINS. `Print(x)` with no namespace resolves to
//!   `pls::builtin::Print(x)` -- but only if the script did not define its
//!   own `Print`. User definitions always win, so adding a builtin in a
//!   later version can never silently hijack a call in an existing script.
//!
//! * `Main` AND `Tick` ARE WIRED UP AUTOMATICALLY. A server script is
//!   structured as "set things up, then hand control to the tick loop",
//!   which is awkward to express when top-level statements are the entry
//!   point: `PlS::Run()` blocks forever, so anything the script wanted to
//!   do per-tick has to have been registered before it. So: if the script
//!   defines a top-level `Tick()`, it is registered as the per-tick handler
//!   before anything runs; if it defines a top-level `Main()`, that is
//!   called after the top-level statements. A script that uses neither
//!   still works -- its top-level statements are simply the program.
//!
//! * OBJECT LITERALS and FOR-IN, which PlainVulkan's grammar does not have.

use pls_transpiler::ast::*;
use std::collections::HashSet;
use std::fmt::Write as _;

use crate::pls_commands;

#[derive(Debug, Clone)]
pub struct CodegenDiagnostic {
    pub message: String,
    pub span: Option<Span>,
}

pub struct CodegenOutput {
    pub cpp: String,
    pub warnings: Vec<CodegenDiagnostic>,
}

/// The engine commands whose parameter is a C++ *function pointer*
/// (`PlayerCallback` / `PlayerCallback2` in pls_server.h) rather than the
/// uniform `Value` every other command takes. Their argument therefore has to
/// be the name of a top-level `function` with the matching parameter count --
/// a literal, a variable, or a nested function (which compiles to a
/// `std::function`, not a raw function pointer) cannot convert. Checking that
/// here turns what would otherwise be an opaque C++ overload error inside
/// generated code into a precise .pls diagnostic.
const CALLBACK_COMMANDS: &[(&str, usize)] = &[
    ("OnPlayerConnect", 1),
    ("OnPlayerDisconnect", 2),
    ("OnPlayerInput", 2),
];

fn callback_arity(name: &str) -> Option<usize> {
    CALLBACK_COMMANDS.iter().find(|(n, _)| *n == name).map(|(_, a)| *a)
}

pub fn generate(program: &Program) -> Result<CodegenOutput, Vec<CodegenDiagnostic>> {
    // Top-level function names are collected up front, before any statement
    // is emitted, because the builtin-shadowing rule has to give the same
    // answer regardless of whether the call site appears above or below the
    // definition that shadows it.
    let mut user_functions = HashSet::new();
    // Same sweep, keeping the parameter count: the callback check and the
    // "function used as a value" check both need arities.
    let mut top_level_fns = std::collections::HashMap::new();
    for stmt in &program.stmts {
        if let Stmt::Function(f) = stmt {
            user_functions.insert(f.name.clone());
            top_level_fns.insert(f.name.clone(), f.params.len());
        }
    }

    let mut cg = Codegen {
        errors: Vec::new(),
        warnings: Vec::new(),
        const_scopes: vec![HashSet::new()],
        user_functions,
        top_level_fns,
        in_main: false,
        loop_counter: 0,
    };
    let cpp = cg.emit_program(program);
    if cg.errors.is_empty() {
        Ok(CodegenOutput { cpp, warnings: cg.warnings })
    } else {
        Err(cg.errors)
    }
}

struct Codegen {
    errors: Vec<CodegenDiagnostic>,
    warnings: Vec<CodegenDiagnostic>,
    /// Stack of lexical scopes tracking which names were declared `const`,
    /// innermost last. Used only for the "assignment to const" diagnostic.
    const_scopes: Vec<HashSet<String>>,
    /// Names of top-level functions, used to let a user definition shadow a
    /// same-named builtin.
    user_functions: HashSet<String>,
    /// Top-level `function` name -> parameter count. Used to validate
    /// callback arguments and to reject using a function as a plain value.
    top_level_fns: std::collections::HashMap<String, usize>,
    /// True while emitting statements that land directly in `int main()`.
    /// `return` is legal at a .pls script's top level, but main() returns
    /// `int`, not `Value`, so those returns need different code than the ones
    /// inside a `function` body.
    in_main: bool,
    /// Makes generated for-in temporaries unique so the loops can nest.
    loop_counter: usize,
}

const PREAMBLE: &str = "\
// Auto-generated by plscc from a PlainS (.pls) source file.
// Do not edit by hand -- re-run `pls build` / `plscc build` instead.
#include <functional>
#include <cmath>
#include <exception>
#include <string>
#include \"pls/pls_value.h\"
#include \"pls/pls_runtime.h\"

using pls::Value;
";

fn indent(level: usize) -> String {
    "    ".repeat(level)
}

fn sanitize_ident(name: &str) -> String {
    format!("u_{}", name)
}

fn cpp_string_literal(s: &str) -> String {
    let mut out = String::from("\"");
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\t' => out.push_str("\\t"),
            '\r' => out.push_str("\\r"),
            '\0' => out.push_str("\\0"),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

fn cpp_float_literal(f: f64) -> String {
    if f.is_nan() {
        return "std::nan(\"\")".to_string();
    }
    if f.is_infinite() {
        return if f > 0.0 { "HUGE_VAL".to_string() } else { "(-HUGE_VAL)".to_string() };
    }
    let mut s = format!("{}", f);
    if !s.contains('.') && !s.contains('e') && !s.contains('E') {
        s.push_str(".0");
    }
    s
}

impl Codegen {
    fn emit_program(&mut self, program: &Program) -> String {
        let mut out = String::new();
        out.push_str(PREAMBLE);
        out.push('\n');

        // Top-level declarations become file-scope globals (so every
        // function can see them), with their *initializers* left at their
        // original position inside main() so initialization order and any
        // side effects match the script's top-to-bottom reading order.
        // Same reasoning as PlainVulkan's codegen; see the long comment
        // there.
        for stmt in &program.stmts {
            if let Stmt::VarDecl(d) = stmt {
                let _ = writeln!(out, "Value {} = Value();", sanitize_ident(&d.name));
                self.declare(&d.name, d.kind == DeclKind::Const);
            }
        }
        out.push('\n');

        // Forward-declare every top-level function so call order in the
        // source (including mutual recursion) never matters.
        for stmt in &program.stmts {
            if let Stmt::Function(f) = stmt {
                let params = f
                    .params
                    .iter()
                    .map(|_| "Value".to_string())
                    .collect::<Vec<_>>()
                    .join(", ");
                let _ = writeln!(out, "Value {}({});", sanitize_ident(&f.name), params);
                self.const_scopes[0].remove(&f.name);
            }
        }
        out.push('\n');

        let mut main_stmts: Vec<&Stmt> = Vec::new();
        for stmt in &program.stmts {
            match stmt {
                Stmt::Function(f) => {
                    self.emit_top_level_function(&mut out, f);
                    out.push('\n');
                }
                other => main_stmts.push(other),
            }
        }

        out.push_str("int main(int argc, char** argv) {\n");
        out.push_str("    (void)argc; (void)argv;\n");
        // The whole script body runs inside a try block. Without it, any
        // exception escaping a runtime call (std::bad_alloc from an absurd
        // array index, a filesystem error, a std::bad_variant_access) reaches
        // std::terminate and the process dies with a bare "Abort" and no
        // diagnostic. For an authoritative game server -- a process expected
        // to stay up for days, whose inputs come from untrusted clients --
        // that is the difference between a logged incident and an unexplained
        // outage that takes every connected player with it.
        out.push_str("    try {\n");

        // Register the per-tick handler before anything else runs. It has to
        // happen before the script's own statements, because those may call
        // PlS::Run(), which never returns.
        if let Some(tick) = self.tick_function(program) {
            out.push_str(
                "        // Registered by plscc: this script defines a top-level Tick(),\n\
                 \x20       // which PlS::Run() calls once per server tick.\n",
            );
            let _ = writeln!(out, "        pls::internal::setTickHandler({});", sanitize_ident(&tick));
        }

        self.in_main = true;
        for stmt in main_stmts {
            if let Stmt::VarDecl(d) = stmt {
                if let Some(init) = &d.init {
                    let init_cpp = self.emit_expr(init);
                    let _ = writeln!(out, "        {} = {};", sanitize_ident(&d.name), init_cpp);
                }
                continue;
            }
            self.emit_stmt(&mut out, stmt, 2, 0);
        }

        if self.has_zero_arg_function(program, "Main") {
            out.push_str(
                "        // Called by plscc: this script defines a top-level Main().\n",
            );
            out.push_str("        u_Main();\n");
        }
        self.in_main = false;

        out.push_str("        return 0;\n");
        out.push_str("    } catch (const std::exception& e) {\n");
        out.push_str("        pls::rt::Print(Value(std::string(\"FATAL: unhandled runtime error: \") + e.what()));\n");
        out.push_str("        return 1;\n");
        out.push_str("    } catch (...) {\n");
        out.push_str("        pls::rt::Print(Value(std::string(\"FATAL: unhandled runtime error (unknown exception type)\")));\n");
        out.push_str("        return 1;\n");
        out.push_str("    }\n");
        out.push_str("}\n");

        out
    }

    /// Returns the name of the script's tick handler if it defined a usable
    /// one. A `Tick` that takes parameters is almost certainly a different
    /// function that happens to share the name, so it is left alone (with a
    /// warning) rather than being wired up and then called with the wrong
    /// signature.
    fn tick_function(&mut self, program: &Program) -> Option<String> {
        for stmt in &program.stmts {
            if let Stmt::Function(f) = stmt {
                if f.name == "Tick" {
                    if f.params.is_empty() {
                        return Some(f.name.clone());
                    }
                    self.warnings.push(CodegenDiagnostic {
                        message: "`Tick` takes parameters, so it was NOT registered as the \
                                  per-tick handler (that handler is called with no arguments). \
                                  Rename it, or remove its parameters if it was meant to be the \
                                  tick handler."
                            .to_string(),
                        span: Some(f.span),
                    });
                    return None;
                }
            }
        }
        None
    }

    fn has_zero_arg_function(&mut self, program: &Program, name: &str) -> bool {
        for stmt in &program.stmts {
            if let Stmt::Function(f) = stmt {
                if f.name == name {
                    if f.params.is_empty() {
                        return true;
                    }
                    self.warnings.push(CodegenDiagnostic {
                        message: format!(
                            "`{}` takes parameters, so it was NOT called automatically at \
                             startup (it would have to be called with no arguments).",
                            name
                        ),
                        span: Some(f.span),
                    });
                    return false;
                }
            }
        }
        false
    }

    fn emit_top_level_function(&mut self, out: &mut String, f: &FunctionDecl) {
        let params = f
            .params
            .iter()
            .map(|p| format!("Value {}", sanitize_ident(p)))
            .collect::<Vec<_>>()
            .join(", ");
        let _ = writeln!(out, "Value {}({}) {{", sanitize_ident(&f.name), params);
        self.push_scope();
        for p in &f.params {
            self.const_scopes.last_mut().unwrap().remove(p);
        }
        for stmt in &f.body.stmts {
            self.emit_stmt(out, stmt, 1, 0);
        }
        self.pop_scope();
        out.push_str("    return Value();\n");
        out.push_str("}\n");
    }

    fn push_scope(&mut self) {
        self.const_scopes.push(HashSet::new());
    }
    fn pop_scope(&mut self) {
        self.const_scopes.pop();
    }
    fn declare(&mut self, name: &str, is_const: bool) {
        if is_const {
            self.const_scopes.last_mut().unwrap().insert(name.to_string());
        } else {
            self.const_scopes.last_mut().unwrap().remove(name);
        }
    }
    fn is_const(&self, name: &str) -> bool {
        self.const_scopes.iter().rev().any(|s| s.contains(name))
    }

    fn root_ident<'a>(&self, lv: &'a LValue) -> &'a str {
        match lv {
            LValue::Ident(n, _) => n,
            LValue::Index(base, _, _) => self.root_ident(base),
            LValue::Member(base, _, _) => self.root_ident(base),
        }
    }

    fn emit_stmt(&mut self, out: &mut String, stmt: &Stmt, ind: usize, loop_depth: usize) {
        let pad = indent(ind);
        match stmt {
            Stmt::VarDecl(d) => {
                let init_cpp = d
                    .init
                    .as_ref()
                    .map(|e| self.emit_expr(e))
                    .unwrap_or_else(|| "Value()".to_string());
                let kw = if d.kind == DeclKind::Const { "const Value" } else { "Value" };
                let _ = writeln!(out, "{}{} {} = {};", pad, kw, sanitize_ident(&d.name), init_cpp);
                self.declare(&d.name, d.kind == DeclKind::Const);
            }
            Stmt::Assign(a) => {
                if self.is_const(self.root_ident(&a.target)) {
                    self.errors.push(CodegenDiagnostic {
                        message: format!(
                            "cannot assign to `{}`: it was declared `const`",
                            self.root_ident(&a.target)
                        ),
                        span: Some(a.span),
                    });
                }
                let value_cpp = self.emit_expr(&a.value);
                let expr = self.emit_assign_as_expr(&a.target, a.op, value_cpp);
                let _ = writeln!(out, "{}{};", pad, expr);
            }
            Stmt::Expr(e) => {
                let _ = writeln!(out, "{}{};", pad, self.emit_expr(&e.expr));
            }
            Stmt::If(s) => {
                let _ = writeln!(out, "{}if ((bool)({})) {{", pad, self.emit_expr(&s.cond));
                self.push_scope();
                for st in &s.then_block.stmts {
                    self.emit_stmt(out, st, ind + 1, loop_depth);
                }
                self.pop_scope();
                let _ = writeln!(out, "{}}}", pad);
                for (cond, block) in &s.elifs {
                    let _ = writeln!(out, "{}else if ((bool)({})) {{", pad, self.emit_expr(cond));
                    self.push_scope();
                    for st in &block.stmts {
                        self.emit_stmt(out, st, ind + 1, loop_depth);
                    }
                    self.pop_scope();
                    let _ = writeln!(out, "{}}}", pad);
                }
                if let Some(else_block) = &s.else_block {
                    let _ = writeln!(out, "{}else {{", pad);
                    self.push_scope();
                    for st in &else_block.stmts {
                        self.emit_stmt(out, st, ind + 1, loop_depth);
                    }
                    self.pop_scope();
                    let _ = writeln!(out, "{}}}", pad);
                }
            }
            Stmt::While(s) => {
                let _ = writeln!(out, "{}while ((bool)({})) {{", pad, self.emit_expr(&s.cond));
                self.push_scope();
                for st in &s.body.stmts {
                    self.emit_stmt(out, st, ind + 1, loop_depth + 1);
                }
                self.pop_scope();
                let _ = writeln!(out, "{}}}", pad);
            }
            Stmt::For(s) => {
                self.push_scope();
                let init_cpp = match &s.init {
                    ForInit::VarDecl(d) => {
                        let init_e = d
                            .init
                            .as_ref()
                            .map(|e| self.emit_expr(e))
                            .unwrap_or_else(|| "Value()".to_string());
                        self.declare(&d.name, d.kind == DeclKind::Const);
                        format!("Value {} = {}", sanitize_ident(&d.name), init_e)
                    }
                    ForInit::Assign(a) => {
                        let value_cpp = self.emit_expr(&a.value);
                        self.emit_assign_as_expr(&a.target, a.op, value_cpp)
                    }
                    ForInit::Expr(e) => self.emit_expr(&e.expr),
                    ForInit::Empty => String::new(),
                };
                let cond_cpp = s.cond.as_ref().map(|c| self.emit_expr(c)).unwrap_or_default();
                let update_cpp = match &s.update {
                    ForUpdate::Assign(a) => {
                        let value_cpp = self.emit_expr(&a.value);
                        self.emit_assign_as_expr(&a.target, a.op, value_cpp)
                    }
                    ForUpdate::Expr(e) => self.emit_expr(&e.expr),
                    ForUpdate::Empty => String::new(),
                };
                let _ = writeln!(out, "{}for ({}; {}; {}) {{", pad, init_cpp, cond_cpp, update_cpp);
                for st in &s.body.stmts {
                    self.emit_stmt(out, st, ind + 1, loop_depth + 1);
                }
                let _ = writeln!(out, "{}}}", pad);
                self.pop_scope();
            }
            Stmt::ForIn(s) => {
                // The iterable is evaluated ONCE into a temporary, and its
                // length read once before the loop starts. Both matter: the
                // iterable is usually a call like PlS::GetConnectedPlayers()
                // that would otherwise re-run every iteration, and a body
                // that kicks a player or destroys an entity must not be able
                // to shift the sequence under the loop. `iter_at` is
                // bounds-checked and yields null past the end, so even a
                // stale count cannot read out of range.
                let id = self.loop_counter;
                self.loop_counter += 1;
                let iter_var = format!("__iter{}", id);
                let count_var = format!("__count{}", id);
                let idx_var = format!("__i{}", id);
                let iterable_cpp = self.emit_expr(&s.iterable);

                let _ = writeln!(out, "{}{{", pad);
                let inner = indent(ind + 1);
                let _ = writeln!(out, "{}Value {} = {};", inner, iter_var, iterable_cpp);
                let _ = writeln!(
                    out,
                    "{}int64_t {} = pls::iter_count({});",
                    inner, count_var, iter_var
                );
                let _ = writeln!(
                    out,
                    "{}for (int64_t {} = 0; {} < {}; ++{}) {{",
                    inner, idx_var, idx_var, count_var, idx_var
                );
                self.push_scope();
                self.declare(&s.var_name, false);
                let _ = writeln!(
                    out,
                    "{}    Value {} = pls::iter_at({}, {});",
                    inner,
                    sanitize_ident(&s.var_name),
                    iter_var,
                    idx_var
                );
                for st in &s.body.stmts {
                    self.emit_stmt(out, st, ind + 2, loop_depth + 1);
                }
                self.pop_scope();
                let _ = writeln!(out, "{}}}", inner);
                let _ = writeln!(out, "{}}}", pad);
            }
            Stmt::Break(span) => {
                if loop_depth == 0 {
                    self.errors.push(CodegenDiagnostic {
                        message: "`break` used outside of a loop".to_string(),
                        span: Some(*span),
                    });
                }
                let _ = writeln!(out, "{}break;", pad);
            }
            Stmt::Continue(span) => {
                if loop_depth == 0 {
                    self.errors.push(CodegenDiagnostic {
                        message: "`continue` used outside of a loop".to_string(),
                        span: Some(*span),
                    });
                }
                let _ = writeln!(out, "{}continue;", pad);
            }
            Stmt::Return(r) => {
                // A `return` at the script's top level is legal PlainS ("stop
                // here"), but it lands in `int main()`, which cannot return a
                // Value -- emitting one produced C++ that simply did not
                // compile. Evaluate the expression (it may have side effects),
                // then use it as the process exit code when it's a number and
                // 0 otherwise, which is what a script author writing
                // `return 1;` means.
                if self.in_main {
                    match &r.value {
                        Some(e) => {
                            let v = self.emit_expr(e);
                            let _ = writeln!(
                                out,
                                "{}{{ Value __ret = {}; return __ret.isNumber() ? \
                                 static_cast<int>(__ret.asInt()) : 0; }}",
                                pad, v
                            );
                        }
                        None => {
                            let _ = writeln!(out, "{}return 0;", pad);
                        }
                    }
                    return;
                }
                let v = r
                    .value
                    .as_ref()
                    .map(|e| self.emit_expr(e))
                    .unwrap_or_else(|| "Value()".to_string());
                let _ = writeln!(out, "{}return {};", pad, v);
            }
            Stmt::Function(f) => {
                // Nested function -> local recursive lambda. `std::function`
                // (rather than `auto`) lets the lambda's own initializer
                // refer to its name for recursion, and `[&]` gives it access
                // to the enclosing scope.
                //
                // Note this is also why a nested function cannot be passed
                // to PlS::OnPlayerConnect and friends: those take a plain
                // function pointer, which a capturing lambda has no
                // conversion to. Callbacks must be top-level functions.
                let param_types = f.params.iter().map(|_| "Value").collect::<Vec<_>>().join(", ");
                let params = f
                    .params
                    .iter()
                    .map(|p| format!("Value {}", sanitize_ident(p)))
                    .collect::<Vec<_>>()
                    .join(", ");
                let _ = writeln!(
                    out,
                    "{}std::function<Value({})> {} = [&]({}) -> Value {{",
                    pad,
                    param_types,
                    sanitize_ident(&f.name),
                    params
                );
                self.push_scope();
                for p in &f.params {
                    self.const_scopes.last_mut().unwrap().remove(p);
                }
                // Inside the lambda we are no longer emitting main()'s body,
                // so `return` here is a normal Value-returning return.
                let outer_in_main = self.in_main;
                self.in_main = false;
                for st in &f.body.stmts {
                    self.emit_stmt(out, st, ind + 1, 0);
                }
                self.in_main = outer_in_main;
                self.pop_scope();
                let _ = writeln!(out, "{}    return Value();", pad);
                let _ = writeln!(out, "{}}};", pad);
            }
        }
    }

    fn emit_lvalue_ref(&mut self, lv: &LValue) -> String {
        match lv {
            LValue::Ident(name, _) => sanitize_ident(name),
            LValue::Index(base, idx, _) => {
                format!("pls::index_ref({}, {})", self.emit_lvalue_ref(base), self.emit_expr(idx))
            }
            LValue::Member(base, field, _) => {
                format!(
                    "pls::member_ref({}, {})",
                    self.emit_lvalue_ref(base),
                    cpp_string_literal(field)
                )
            }
        }
    }

    fn emit_assign_as_expr(&mut self, target: &LValue, op: AssignOp, value_cpp: String) -> String {
        let target_ref = self.emit_lvalue_ref(target);
        let body = match op {
            AssignOp::Assign => format!("__lv = {};", value_cpp),
            AssignOp::AddAssign => format!("__lv = __lv + ({});", value_cpp),
            AssignOp::SubAssign => format!("__lv = __lv - ({});", value_cpp),
            AssignOp::MulAssign => format!("__lv = __lv * ({});", value_cpp),
            AssignOp::DivAssign => format!("__lv = __lv / ({});", value_cpp),
            AssignOp::ModAssign => format!("__lv = __lv % ({});", value_cpp),
        };
        format!("([&]() -> Value {{ Value& __lv = {}; {} return __lv; }})()", target_ref, body)
    }

    fn emit_expr(&mut self, e: &Expr) -> String {
        match e {
            Expr::IntLiteral(n, _) => format!("Value(static_cast<int64_t>({}))", n),
            Expr::FloatLiteral(f, _) => format!("Value({})", cpp_float_literal(*f)),
            Expr::StringLiteral(s, _) => format!("Value(std::string({}))", cpp_string_literal(s)),
            Expr::BoolLiteral(b, _) => format!("Value({})", b),
            Expr::NullLiteral(_) => "Value()".to_string(),
            Expr::ArrayLiteral(elems, _) => {
                let items = elems.iter().map(|el| self.emit_expr(el)).collect::<Vec<_>>().join(", ");
                format!("pls::make_array({{{}}})", items)
            }
            Expr::ObjectLiteral(fields, span) => {
                // Duplicate keys would silently drop a field, and in a
                // packet-building literal that is a bug the script author
                // almost never intends -- so it is worth a warning even
                // though the C++ is perfectly well-defined (last wins).
                let mut seen: HashSet<&str> = HashSet::new();
                for (k, _) in fields {
                    if !seen.insert(k.as_str()) {
                        self.warnings.push(CodegenDiagnostic {
                            message: format!(
                                "duplicate key `{}` in object literal; the last value wins and \
                                 the earlier one is discarded",
                                k
                            ),
                            span: Some(*span),
                        });
                    }
                }
                let items = fields
                    .iter()
                    .map(|(k, v)| format!("{{{}, {}}}", cpp_string_literal(k), self.emit_expr(v)))
                    .collect::<Vec<_>>()
                    .join(", ");
                format!("pls::make_object({{{}}})", items)
            }
            Expr::Ident(name, span) => {
                // A bare top-level function name used where a value is
                // expected. C++ would silently convert the function pointer to
                // `bool` via Value(bool) and hand the runtime `true` -- no
                // error, just wrong behaviour that is very hard to trace back.
                // The only position where a function name is meaningful is a
                // callback argument, and that path never reaches emit_expr
                // (see emit_callback_call).
                if self.top_level_fns.contains_key(name) {
                    self.errors.push(CodegenDiagnostic {
                        message: format!(
                            "`{}` is a function, and PlainS values can't hold a function. Did \
                             you mean to call it -- `{}(...)`? (Passing a function by name is \
                             only supported for the player callback commands: {}.)",
                            name,
                            name,
                            CALLBACK_COMMANDS
                                .iter()
                                .map(|(n, _)| format!("PlS::{}", n))
                                .collect::<Vec<_>>()
                                .join(", ")
                        ),
                        span: Some(*span),
                    });
                }
                sanitize_ident(name)
            }
            Expr::Index(base, idx, _) => {
                format!("pls::index_get({}, {})", self.emit_expr(base), self.emit_expr(idx))
            }
            Expr::Member(base, field, _) => {
                format!("pls::member_get({}, {})", self.emit_expr(base), cpp_string_literal(field))
            }
            Expr::Scope(_base, name, span) => {
                self.warnings.push(CodegenDiagnostic {
                    message: format!(
                        "`{}` is referenced without being called; PlainS's runtime values can't \
                         hold a bare engine-function reference, so this becomes `null`. Did you \
                         mean to call it, e.g. `PlS::{}(...)`?",
                        name, name
                    ),
                    span: Some(*span),
                });
                "Value()".to_string()
            }
            Expr::Call(callee, args, span) => self.emit_call(callee, args, *span),
            Expr::Unary(op, operand, _) => {
                let o = self.emit_expr(operand);
                match op {
                    UnaryOp::Neg => format!("(-{})", o),
                    UnaryOp::Not => format!("(!{})", o),
                    UnaryOp::BitNot => format!("(~{})", o),
                }
            }
            Expr::Binary(op, l, r, _) => {
                let lc = self.emit_expr(l);
                let rc = self.emit_expr(r);
                match op {
                    BinaryOp::Add => format!("({} + {})", lc, rc),
                    BinaryOp::Sub => format!("({} - {})", lc, rc),
                    BinaryOp::Mul => format!("({} * {})", lc, rc),
                    BinaryOp::Div => format!("({} / {})", lc, rc),
                    BinaryOp::Mod => format!("({} % {})", lc, rc),
                    BinaryOp::Eq => format!("({} == {})", lc, rc),
                    BinaryOp::Ne => format!("({} != {})", lc, rc),
                    BinaryOp::Lt => format!("({} < {})", lc, rc),
                    BinaryOp::Gt => format!("({} > {})", lc, rc),
                    BinaryOp::Le => format!("({} <= {})", lc, rc),
                    BinaryOp::Ge => format!("({} >= {})", lc, rc),
                    // Short-circuiting: see the note on Value's missing
                    // operator&&/|| in pls_value.h.
                    BinaryOp::And => format!("Value((bool)({}) && (bool)({}))", lc, rc),
                    BinaryOp::Or => format!("Value((bool)({}) || (bool)({}))", lc, rc),
                }
            }
            Expr::Ternary(c, t, f, _) => format!(
                "((bool)({}) ? ({}) : ({}))",
                self.emit_expr(c),
                self.emit_expr(t),
                self.emit_expr(f)
            ),
            Expr::Assign(target, op, value, _) => {
                let value_cpp = self.emit_expr(value);
                if self.is_const(self.root_ident(target)) {
                    self.errors.push(CodegenDiagnostic {
                        message: format!(
                            "cannot assign to `{}`: it was declared `const`",
                            self.root_ident(target)
                        ),
                        span: Some(e.span()),
                    });
                }
                self.emit_assign_as_expr(target, *op, value_cpp)
            }
        }
    }

    /// Emits one of the player callback commands. Their runtime signature
    /// takes a raw `Value(*)(...)` function pointer, so the only thing that
    /// can legally be passed is the name of a *top-level* `function` with the
    /// right parameter count. Everything else is rejected here with a
    /// source-level message rather than being handed to the C++ compiler,
    /// which would report it as an unreadable "cannot convert 'pls::Value' to
    /// 'pls::PlayerCallback'" against generated code the user never wrote.
    fn emit_callback_call(
        &mut self,
        name: &str,
        cb_arity: usize,
        args: &[Expr],
        span: Span,
    ) -> String {
        let param_hint = match cb_arity {
            1 => "player_id",
            2 if name == "OnPlayerDisconnect" => "player_id, reason",
            2 => "player_id, input",
            _ => "",
        };
        if args.len() != 1 {
            self.errors.push(CodegenDiagnostic {
                message: format!(
                    "PlS::{} expects 1 argument (a callback function), got {}",
                    name,
                    args.len()
                ),
                span: Some(span),
            });
            return "Value()".to_string();
        }
        let arg_name = match &args[0] {
            Expr::Ident(n, _) => n.clone(),
            _ => {
                self.errors.push(CodegenDiagnostic {
                    message: format!(
                        "PlS::{} takes a callback *function*, so its argument must be the name \
                         of a top-level `function` -- not an expression or literal. Write \
                         `function MyHandler({}) {{ ... }}` and pass `PlS::{}(MyHandler);`",
                        name, param_hint, name
                    ),
                    span: Some(span),
                });
                return "Value()".to_string();
            }
        };
        match self.top_level_fns.get(&arg_name) {
            None => {
                self.errors.push(CodegenDiagnostic {
                    message: format!(
                        "PlS::{} takes a callback function, but `{}` is not a top-level \
                         `function` in this file. (A variable can't hold a function, and a \
                         function nested inside another compiles to a closure, which the \
                         server's callback slot cannot store -- declare `{}` at the top level.)",
                        name, arg_name, arg_name
                    ),
                    span: Some(span),
                });
                return "Value()".to_string();
            }
            Some(&declared) if declared != cb_arity => {
                self.errors.push(CodegenDiagnostic {
                    message: format!(
                        "PlS::{} calls its callback with {} argument(s) ({}), but `{}` declares \
                         {}. Change `{}` to take exactly {} parameter(s).",
                        name, cb_arity, param_hint, arg_name, declared, arg_name, cb_arity
                    ),
                    span: Some(span),
                });
                return "Value()".to_string();
            }
            Some(_) => {}
        }
        format!("pls::rt::{}({})", name, sanitize_ident(&arg_name))
    }

    fn emit_call(&mut self, callee: &Expr, args: &[Expr], span: Span) -> String {
        if let Expr::Scope(base, name, _) = callee {
            if let Expr::Ident(ns, _) = base.as_ref() {
                // Callback commands take a function pointer, so their argument
                // must NOT go through emit_expr (which would wrap it in a
                // Value). Validate and emit the bare name instead.
                if ns == "PlS" {
                    if let Some(cb_arity) = callback_arity(name) {
                        return self.emit_callback_call(name, cb_arity, args, span);
                    }
                }
                let args_cpp: Vec<String> = args.iter().map(|a| self.emit_expr(a)).collect();
                if ns == "PlS" {
                    match pls_commands::lookup(name) {
                        Some(sig) if sig.arity != args.len() => {
                            self.errors.push(CodegenDiagnostic {
                                message: format!(
                                    "PlS::{} expects {} argument(s), got {}",
                                    name,
                                    sig.arity,
                                    args.len()
                                ),
                                span: Some(span),
                            });
                        }
                        Some(_) => {}
                        None => {
                            self.warnings.push(CodegenDiagnostic {
                                message: format!(
                                    "PlS::{} is not a recognized server command (typo, or a \
                                     newer runtime function this compiler doesn't know about \
                                     yet); emitting the call and letting the linker decide",
                                    name
                                ),
                                span: Some(span),
                            });
                        }
                    }
                    return format!("pls::rt::{}({})", name, args_cpp.join(", "));
                }
                // Any other namespace is emitted as a literal C++ scoped
                // call -- an intentional extension point for a user who adds
                // `namespace MyLib { Value Thing(...); }` to a header on the
                // runtime's include path.
                return format!("{}::{}({})", ns, name, args_cpp.join(", "));
            }
        }
        if let Expr::Ident(name, _) = callee {
            let args_cpp: Vec<String> = args.iter().map(|a| self.emit_expr(a)).collect();
            // A user-defined function always shadows a builtin of the same
            // name, so a script that defines its own Print keeps it.
            if !self.user_functions.contains(name) {
                if let Some(sig) = pls_commands::lookup_builtin(name) {
                    if sig.arity != args.len() {
                        self.errors.push(CodegenDiagnostic {
                            message: format!(
                                "builtin {} expects {} argument(s), got {}",
                                name,
                                sig.arity,
                                args.len()
                            ),
                            span: Some(span),
                        });
                    }
                    return format!("pls::builtin::{}({})", name, args_cpp.join(", "));
                }
            }
            return format!("{}({})", sanitize_ident(name), args_cpp.join(", "));
        }
        self.errors.push(CodegenDiagnostic {
            message: "calling a non-identifier, non-`Namespace::Function` expression isn't \
                       supported by the code generator (PlainS values can't hold callable \
                       closures, so e.g. `arr[0]()` has no runtime representation to call)"
                .to_string(),
            span: Some(span),
        });
        "Value()".to_string()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Generate C++ for a .pls source, expecting success.
    fn gen_ok(src: &str) -> String {
        let program = pls_transpiler::parse(src).expect("source should parse");
        match generate(&program) {
            Ok(out) => out.cpp,
            Err(errs) => panic!(
                "expected codegen to succeed, got: {:?}",
                errs.iter().map(|e| &e.message).collect::<Vec<_>>()
            ),
        }
    }

    /// Generate C++ for a .pls source, expecting an error containing `needle`.
    fn gen_err(src: &str, needle: &str) {
        let program = pls_transpiler::parse(src).expect("source should parse");
        match generate(&program) {
            Ok(_) => panic!("expected a codegen error mentioning {:?}, but generation succeeded", needle),
            Err(errs) => {
                let joined = errs.iter().map(|e| e.message.clone()).collect::<Vec<_>>().join("\n");
                assert!(
                    joined.contains(needle),
                    "expected an error mentioning {:?}, got:\n{}",
                    needle,
                    joined
                );
            }
        }
    }

    // ---- top-level `return` -------------------------------------------
    // main() returns int, so a top-level `return` must not emit
    // `return Value(...)`. That produced C++ that did not compile at all.

    #[test]
    fn top_level_return_with_value_is_an_int_exit_code() {
        let cpp = gen_ok("Print(1);\nreturn 2;\n");
        assert!(!cpp.contains("return Value(static_cast<int64_t>(2));"),
                "must not return a Value from main:\n{}", cpp);
        assert!(cpp.contains("static_cast<int>(__ret.asInt())"),
                "expected an int exit-code return:\n{}", cpp);
    }

    #[test]
    fn top_level_bare_return_returns_zero() {
        let cpp = gen_ok("Print(1);\nreturn;\n");
        assert!(cpp.contains("return 0;"), "{}", cpp);
    }

    #[test]
    fn top_level_return_inside_an_if_still_returns_int() {
        let cpp = gen_ok("if (true) { return 1; }\n");
        assert!(cpp.contains("static_cast<int>(__ret.asInt())"),
                "a return nested in a block at top level is still main's return:\n{}", cpp);
    }

    #[test]
    fn return_inside_a_function_still_returns_a_value() {
        let cpp = gen_ok("function F() { return 5; }\n");
        assert!(cpp.contains("return Value(static_cast<int64_t>(5));"),
                "function returns stay Value-typed:\n{}", cpp);
    }

    #[test]
    fn return_inside_a_nested_function_still_returns_a_value() {
        // `.pls` has no bare block statement, so the nesting is inside a
        // function rather than a `{ ... }`.
        let cpp = gen_ok("function Outer() { function G() { return 7; } }\n");
        assert!(cpp.contains("return Value(static_cast<int64_t>(7));"),
                "nested-function returns stay Value-typed:\n{}", cpp);
    }

    // ---- main() exception guard ---------------------------------------
    // A server is a long-running process fed by untrusted clients; an
    // unhandled exception must be logged, not an unexplained abort.

    #[test]
    fn main_body_is_wrapped_in_an_exception_guard() {
        let cpp = gen_ok("Print(1);\n");
        assert!(cpp.contains("try {"), "main should open a try block:\n{}", cpp);
        assert!(cpp.contains("catch (const std::exception& e)"), "{}", cpp);
        assert!(cpp.contains("catch (...)"), "{}", cpp);
    }

    #[test]
    fn tick_handler_registration_is_inside_the_guard() {
        let cpp = gen_ok("function Tick() { Print(1); }\n");
        let try_at = cpp.find("try {").expect("expected a try block");
        let reg_at = cpp.find("setTickHandler").expect("expected tick registration");
        assert!(try_at < reg_at, "tick registration must sit inside the guard:\n{}", cpp);
    }

    // ---- player callback commands --------------------------------------
    // These take a C++ function pointer, so their argument must be a
    // top-level function of matching arity. Anything else used to reach the
    // C++ compiler as an unreadable conversion error.

    #[test]
    fn callback_accepts_a_matching_top_level_function() {
        let cpp = gen_ok("function OnC(id) { Print(id); }\nPlS::OnPlayerConnect(OnC);\n");
        assert!(cpp.contains("pls::rt::OnPlayerConnect(u_OnC)"),
                "callback should be passed as a bare function name:\n{}", cpp);
        assert!(!cpp.contains("OnPlayerConnect(Value"),
                "callback argument must not be wrapped in a Value:\n{}", cpp);
    }

    #[test]
    fn two_arg_callbacks_accept_two_arg_functions() {
        let cpp = gen_ok(
            "function OnD(id, reason) { Print(reason); }\n\
             function OnI(id, input) { Print(input); }\n\
             PlS::OnPlayerDisconnect(OnD);\n\
             PlS::OnPlayerInput(OnI);\n",
        );
        assert!(cpp.contains("pls::rt::OnPlayerDisconnect(u_OnD)"), "{}", cpp);
        assert!(cpp.contains("pls::rt::OnPlayerInput(u_OnI)"), "{}", cpp);
    }

    #[test]
    fn callback_rejects_a_literal() {
        gen_err("PlS::OnPlayerConnect(1);\n", "must be the name of a top-level `function`");
    }

    #[test]
    fn callback_rejects_an_expression() {
        gen_err("var x = 1;\nPlS::OnPlayerConnect(x + 1);\n", "must be the name of a top-level `function`");
    }

    #[test]
    fn callback_rejects_an_undeclared_name() {
        gen_err("PlS::OnPlayerInput(NoSuchFn);\n", "is not a top-level `function`");
    }

    #[test]
    fn callback_rejects_a_variable() {
        gen_err("var handler = 1;\nPlS::OnPlayerInput(handler);\n", "is not a top-level `function`");
    }

    #[test]
    fn callback_rejects_a_nested_function() {
        // A nested function compiles to a std::function closure, which has no
        // conversion to a raw function pointer.
        gen_err(
            "function Outer() { function Inner(id) { Print(id); } }\nPlS::OnPlayerConnect(Inner);\n",
            "is not a top-level `function`",
        );
    }

    #[test]
    fn callback_rejects_wrong_parameter_count() {
        gen_err("function H() { Print(1); }\nPlS::OnPlayerConnect(H);\n", "declares 0");
    }

    #[test]
    fn two_arg_callback_rejects_a_one_arg_function() {
        gen_err("function H(a) { Print(a); }\nPlS::OnPlayerInput(H);\n", "declares 1");
    }

    // ---- functions used as values --------------------------------------

    #[test]
    fn function_used_as_a_value_is_rejected() {
        gen_err("function H() { Print(1); }\nPrint(H);\n", "can't hold a function");
    }

    #[test]
    fn function_assigned_to_a_variable_is_rejected() {
        gen_err("function H() { Print(1); }\nvar f = H;\n", "can't hold a function");
    }

    #[test]
    fn calling_a_function_normally_still_works() {
        let cpp = gen_ok("function H() { Print(1); }\nH();\n");
        assert!(cpp.contains("u_H()"), "a normal call is unaffected:\n{}", cpp);
    }

    // ---- things that must keep working ---------------------------------

    #[test]
    fn arity_mismatch_on_a_normal_command_is_still_caught() {
        gen_err("PlS::SetEntityPosition(1);\n", "expects 4 argument(s), got 1");
    }

    #[test]
    fn object_literals_and_for_in_still_compile() {
        let cpp = gen_ok(
            "var o = {\"a\": 1, \"b\": 2};\n\
             var xs = [1, 2, 3];\n\
             for (x in xs) { Print(x); }\n",
        );
        assert!(cpp.contains("make_object"), "object literal should compile:\n{}", cpp);
        assert!(cpp.contains("iter_at"), "for-in should compile:\n{}", cpp);
    }

    #[test]
    fn the_mp_server_examples_shape_compiles() {
        // The exact wiring the shipped example uses: Main + Tick + all three
        // player callbacks. This is the shape a real server has.
        let cpp = gen_ok(
            "function Main() { PlS::Listen(8080); PlS::Run(); }\n\
             function OnC(id) { Print(id); }\n\
             function OnD(id, r) { Print(r); }\n\
             function OnI(id, i) { Print(i); }\n\
             function Tick() { PlS::BroadcastStateSelective(); }\n",
        );
        assert!(cpp.contains("setTickHandler(u_Tick)"), "{}", cpp);
        assert!(cpp.contains("u_Main();"), "{}", cpp);
    }
}
