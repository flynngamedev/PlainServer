//! Token types produced by the hand-written lexer and consumed by the
//! LALRPOP-generated parser via the "external tokenizer" mechanism.
//!
//! We use a hand-written lexer (rather than LALRPOP's built-in regex lexer)
//! because PlainS needs lexing behavior that's awkward to express as
//! pure regex priority rules: distinguishing `.` (member access) from the
//! decimal point in `3.14`, hex/binary integer literals, scientific-notation
//! floats, escaped string literals, and both `//` / `/* */` comment styles.

use std::fmt;

#[derive(Debug, Clone, PartialEq)]
pub enum Tok {
    // Literals
    Int(i64),
    Float(f64),
    Str(String),
    Ident(String),

    // Keywords
    KwVar,
    KwLet,
    KwConst,
    KwFunction,
    KwReturn,
    KwIf,
    KwElif,
    KwElse,
    KwWhile,
    KwFor,
    KwIn,
    KwBreak,
    KwContinue,
    KwTrue,
    KwFalse,
    KwNull,

    // Punctuation
    LBrace,    // {
    RBrace,    // }
    LParen,    // (
    RParen,    // )
    LBracket,  // [
    RBracket,  // ]
    Comma,     // ,
    Semi,      // ;
    Dot,       // .
    ColonColon, // ::
    Colon,     // :
    Question,  // ?

    // Assignment operators
    Assign,    // =
    PlusEq,    // +=
    MinusEq,   // -=
    StarEq,    // *=
    SlashEq,   // /=
    PercentEq, // %=

    // Comparison operators
    EqEq,  // ==
    NotEq, // !=
    Lt,    // <
    Gt,    // >
    LtEq,  // <=
    GtEq,  // >=

    // Logical operators
    AndAnd, // &&
    OrOr,   // ||
    Bang,   // !

    // Arithmetic / unary operators
    Plus,    // +
    Minus,   // -
    Star,    // *
    Slash,   // /
    Percent, // %
    Tilde,   // ~
}

impl fmt::Display for Tok {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Tok::Int(n) => write!(f, "{}", n),
            Tok::Float(n) => write!(f, "{}", n),
            Tok::Str(s) => write!(f, "{:?}", s),
            Tok::Ident(s) => write!(f, "{}", s),
            Tok::KwVar => write!(f, "var"),
            Tok::KwLet => write!(f, "let"),
            Tok::KwConst => write!(f, "const"),
            Tok::KwFunction => write!(f, "function"),
            Tok::KwReturn => write!(f, "return"),
            Tok::KwIf => write!(f, "if"),
            Tok::KwElif => write!(f, "elif"),
            Tok::KwElse => write!(f, "else"),
            Tok::KwWhile => write!(f, "while"),
            Tok::KwFor => write!(f, "for"),
            Tok::KwIn => write!(f, "in"),
            Tok::KwBreak => write!(f, "break"),
            Tok::KwContinue => write!(f, "continue"),
            Tok::KwTrue => write!(f, "true"),
            Tok::KwFalse => write!(f, "false"),
            Tok::KwNull => write!(f, "null"),
            Tok::LBrace => write!(f, "{{"),
            Tok::RBrace => write!(f, "}}"),
            Tok::LParen => write!(f, "("),
            Tok::RParen => write!(f, ")"),
            Tok::LBracket => write!(f, "["),
            Tok::RBracket => write!(f, "]"),
            Tok::Comma => write!(f, ","),
            Tok::Semi => write!(f, ";"),
            Tok::Dot => write!(f, "."),
            Tok::ColonColon => write!(f, "::"),
            Tok::Colon => write!(f, ":"),
            Tok::Question => write!(f, "?"),
            Tok::Assign => write!(f, "="),
            Tok::PlusEq => write!(f, "+="),
            Tok::MinusEq => write!(f, "-="),
            Tok::StarEq => write!(f, "*="),
            Tok::SlashEq => write!(f, "/="),
            Tok::PercentEq => write!(f, "%="),
            Tok::EqEq => write!(f, "=="),
            Tok::NotEq => write!(f, "!="),
            Tok::Lt => write!(f, "<"),
            Tok::Gt => write!(f, ">"),
            Tok::LtEq => write!(f, "<="),
            Tok::GtEq => write!(f, ">="),
            Tok::AndAnd => write!(f, "&&"),
            Tok::OrOr => write!(f, "||"),
            Tok::Bang => write!(f, "!"),
            Tok::Plus => write!(f, "+"),
            Tok::Minus => write!(f, "-"),
            Tok::Star => write!(f, "*"),
            Tok::Slash => write!(f, "/"),
            Tok::Percent => write!(f, "%"),
            Tok::Tilde => write!(f, "~"),
        }
    }
}

pub fn lookup_keyword(ident: &str) -> Option<Tok> {
    Some(match ident {
        "var" => Tok::KwVar,
        "let" => Tok::KwLet,
        "const" => Tok::KwConst,
        "function" => Tok::KwFunction,
        "return" => Tok::KwReturn,
        "if" => Tok::KwIf,
        "elif" => Tok::KwElif,
        "else" => Tok::KwElse,
        "while" => Tok::KwWhile,
        "for" => Tok::KwFor,
        "in" => Tok::KwIn,
        "break" => Tok::KwBreak,
        "continue" => Tok::KwContinue,
        "true" => Tok::KwTrue,
        "false" => Tok::KwFalse,
        "null" => Tok::KwNull,
        _ => return None,
    })
}
