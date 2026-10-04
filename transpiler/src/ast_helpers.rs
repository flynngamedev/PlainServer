//! Small helpers used by `pls.lalrpop`'s semantic actions.
//!
//! LALRPOP grammar files only accept `use`-style items above `grammar;`,
//! not free function definitions, so these live here instead and are
//! imported by the grammar with a plain `use`.

use crate::ast::{AssignStmt, Expr, ExprStmt, ForInit, ForUpdate, Span, Stmt};

/// A top-level `Expr` that turns out to be `Expr::Assign(...)` becomes the
/// dedicated `Stmt::Assign` node (with its strongly-typed `LValue` target)
/// instead of a generic `Stmt::Expr`. Anything else (a bare call like
/// `PlS::Log("hi");`, or any other expression used for its side effects)
/// becomes `Stmt::Expr`.
pub fn stmt_from_expr(e: Expr, span: Span) -> Stmt {
    match e {
        Expr::Assign(target, op, value, _) => Stmt::Assign(AssignStmt {
            target: *target,
            op,
            value: *value,
            span,
        }),
        other => Stmt::Expr(ExprStmt { expr: other, span }),
    }
}

pub fn forinit_from_expr(e: Expr, span: Span) -> ForInit {
    match e {
        Expr::Assign(target, op, value, _) => ForInit::Assign(AssignStmt {
            target: *target,
            op,
            value: *value,
            span,
        }),
        other => ForInit::Expr(ExprStmt { expr: other, span }),
    }
}

pub fn forupdate_from_expr(e: Expr, span: Span) -> ForUpdate {
    match e {
        Expr::Assign(target, op, value, _) => ForUpdate::Assign(AssignStmt {
            target: *target,
            op,
            value: *value,
            span,
        }),
        other => ForUpdate::Expr(ExprStmt { expr: other, span }),
    }
}
