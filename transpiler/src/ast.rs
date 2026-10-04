//! Abstract syntax tree for PlainS.
//!
//! Every syntax construct in the `.pls` language reference has a
//! corresponding node here EXCEPT comments, which are discarded during
//! lexing and never reach the parser at all.
//!
//! Design notes:
//! - `PlS::DrawTriangle(...)` is *not* special-cased into the grammar or the
//!   AST as 150 individual node types. `::` is a general scope-access
//!   operator (`Expr::Scope`), so `PlS::Foo(args)` parses uniformly as
//!   `Expr::Call(Expr::Scope(Expr::Ident("Pv"), "Foo"), args)` -- the same
//!   path any `Namespace::Function(...)` call would take. This keeps the
//!   grammar generic and means new engine commands never require grammar
//!   changes.
//! - Assignment (`a = b`, `a[i] = b`, `a.m = b`) is a statement whose target
//!   is a restricted `LValue`, not an arbitrary `Expr`. The parser builds an
//!   ordinary `Expr` for the left-hand side and then narrows it into an
//!   `LValue`, producing a clean diagnostic if the left-hand side isn't a
//!   valid assignment target (e.g. `1 + 2 = 3;`).

use serde::Serialize;

pub type Span = (usize, usize);

/// A program is simply a sequence of top-level statements. Function
/// declarations are just one more `Stmt` variant (`Stmt::Function`), which
/// is what lets them appear equally validly at the top level or nested
/// inside a block -- there's no separate "Item" wrapper duplicating
/// `Stmt`'s own function-declaration case.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Program {
    pub stmts: Vec<Stmt>,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct FunctionDecl {
    pub name: String,
    pub params: Vec<String>,
    pub body: Block,
    pub span: Span,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Block {
    pub stmts: Vec<Stmt>,
    pub span: Span,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub enum Stmt {
    VarDecl(VarDeclStmt),
    Assign(AssignStmt),
    Expr(ExprStmt),
    If(IfStmt),
    While(WhileStmt),
    For(ForStmt),
    ForIn(ForInStmt),
    Break(Span),
    Continue(Span),
    Return(ReturnStmt),
    Function(FunctionDecl),
}

impl Stmt {
    pub fn span(&self) -> Span {
        match self {
            Stmt::VarDecl(s) => s.span,
            Stmt::Assign(s) => s.span,
            Stmt::Expr(s) => s.span,
            Stmt::If(s) => s.span,
            Stmt::While(s) => s.span,
            Stmt::For(s) => s.span,
            Stmt::ForIn(s) => s.span,
            Stmt::Break(s) => *s,
            Stmt::Continue(s) => *s,
            Stmt::Return(s) => s.span,
            Stmt::Function(f) => f.span,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub enum DeclKind {
    Var,
    Let,
    Const,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct VarDeclStmt {
    pub kind: DeclKind,
    pub name: String,
    pub init: Option<Expr>,
    pub span: Span,
}

/// A restricted subset of `Expr` that is legal on the left-hand side of an
/// assignment: an identifier, optionally followed by any chain of index
/// (`[expr]`) and member (`.ident`) accesses. This mirrors exactly the three
/// forms shown in the language spec: `name = v`, `name[index] = v`, and
/// `object.member = v` (which chain arbitrarily, e.g. `a.b[0].c = v`).
#[derive(Debug, Clone, PartialEq, Serialize)]
pub enum LValue {
    Ident(String, Span),
    Index(Box<LValue>, Box<Expr>, Span),
    Member(Box<LValue>, String, Span),
}

impl LValue {
    pub fn span(&self) -> Span {
        match self {
            LValue::Ident(_, s) => *s,
            LValue::Index(_, _, s) => *s,
            LValue::Member(_, _, s) => *s,
        }
    }

    /// Convert a general expression into an LValue, if it is one.
    /// Used by the grammar's semantic action for assignment statements.
    pub fn from_expr(e: Expr) -> Result<LValue, Span> {
        match e {
            Expr::Ident(name, span) => Ok(LValue::Ident(name, span)),
            Expr::Index(base, idx, span) => {
                let base_lv = LValue::from_expr(*base)?;
                Ok(LValue::Index(Box::new(base_lv), idx, span))
            }
            Expr::Member(base, member, span) => {
                let base_lv = LValue::from_expr(*base)?;
                Ok(LValue::Member(Box::new(base_lv), member, span))
            }
            other => Err(other.span()),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub enum AssignOp {
    Assign,
    AddAssign,
    SubAssign,
    MulAssign,
    DivAssign,
    ModAssign,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct AssignStmt {
    pub target: LValue,
    pub op: AssignOp,
    pub value: Expr,
    pub span: Span,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct ExprStmt {
    pub expr: Expr,
    pub span: Span,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct IfStmt {
    pub cond: Expr,
    pub then_block: Block,
    pub elifs: Vec<(Expr, Block)>,
    pub else_block: Option<Block>,
    pub span: Span,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct WhileStmt {
    pub cond: Expr,
    pub body: Block,
    pub span: Span,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub enum ForInit {
    VarDecl(VarDeclStmt),
    Assign(AssignStmt),
    Expr(ExprStmt),
    Empty,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub enum ForUpdate {
    Assign(AssignStmt),
    Expr(ExprStmt),
    Empty,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct ForStmt {
    pub init: ForInit,
    pub cond: Option<Expr>,
    pub update: ForUpdate,
    pub body: Block,
    pub span: Span,
}

/// `for (name in iterable) { body }` -- iterates an array's elements or
/// an object's keys. Kept as its own node rather than desugaring to a
/// counted `ForStmt` in the parser so the code generator can emit the
/// loop over whichever container the value turns out to hold at
/// runtime, which is not knowable at parse time in an untyped language.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct ForInStmt {
    pub var_name: String,
    pub iterable: Expr,
    pub body: Block,
    pub span: Span,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct ReturnStmt {
    pub value: Option<Expr>,
    pub span: Span,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub enum UnaryOp {
    Neg,    // -x
    Not,    // !x
    BitNot, // ~x
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub enum BinaryOp {
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Eq,
    Ne,
    Lt,
    Gt,
    Le,
    Ge,
    And,
    Or,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub enum Expr {
    IntLiteral(i64, Span),
    FloatLiteral(f64, Span),
    StringLiteral(String, Span),
    BoolLiteral(bool, Span),
    NullLiteral(Span),
    ArrayLiteral(Vec<Expr>, Span),
    /// `{ "key": expr, ... }`. PlainS has this where PlainVulkan does
    /// not, because every server command that sends something to a
    /// client takes a structured payload -- building those by
    /// auto-vivified field assignment would dominate every script.
    /// Keys are stored as plain Strings: they are always literal in the
    /// syntax, so a computed key uses `obj[expr] = v` instead.
    ObjectLiteral(Vec<(String, Expr)>, Span),
    Ident(String, Span),
    Index(Box<Expr>, Box<Expr>, Span),
    Member(Box<Expr>, String, Span),
    /// `base::name`, e.g. `PlS::DrawTriangle` before it's called.
    Scope(Box<Expr>, String, Span),
    Call(Box<Expr>, Vec<Expr>, Span),
    Unary(UnaryOp, Box<Expr>, Span),
    Binary(BinaryOp, Box<Expr>, Box<Expr>, Span),
    Ternary(Box<Expr>, Box<Expr>, Box<Expr>, Span),
    /// Assignment used as an expression (e.g. the right-associative chain
    /// `a = b = 5`). A plain top-level `target = value;` statement is
    /// unwrapped into `Stmt::Assign` by the grammar instead of staying as
    /// this variant -- see `pls.lalrpop`'s `Stmt` rule.
    Assign(Box<LValue>, AssignOp, Box<Expr>, Span),
}

impl Expr {
    pub fn span(&self) -> Span {
        match self {
            Expr::IntLiteral(_, s) => *s,
            Expr::FloatLiteral(_, s) => *s,
            Expr::StringLiteral(_, s) => *s,
            Expr::BoolLiteral(_, s) => *s,
            Expr::NullLiteral(s) => *s,
            Expr::ArrayLiteral(_, s) => *s,
            Expr::ObjectLiteral(_, s) => *s,
            Expr::Ident(_, s) => *s,
            Expr::Index(_, _, s) => *s,
            Expr::Member(_, _, s) => *s,
            Expr::Scope(_, _, s) => *s,
            Expr::Call(_, _, s) => *s,
            Expr::Unary(_, _, s) => *s,
            Expr::Binary(_, _, _, s) => *s,
            Expr::Ternary(_, _, _, s) => *s,
            Expr::Assign(_, _, _, s) => *s,
        }
    }

    /// True if this call expression's callee is `Base::Name` -- i.e. a
    /// namespaced engine command like `PlS::DrawTriangle(...)`. Returns the
    /// (namespace, name) pair when it matches.
    pub fn as_scoped_call(&self) -> Option<(&str, &str, &[Expr])> {
        if let Expr::Call(callee, args, _) = self {
            if let Expr::Scope(base, name, _) = callee.as_ref() {
                if let Expr::Ident(ns, _) = base.as_ref() {
                    return Some((ns.as_str(), name.as_str(), args.as_slice()));
                }
            }
        }
        None
    }
}
