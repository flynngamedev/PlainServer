//! Human-friendly diagnostics: turns byte offsets into 1-based line/column
//! pairs and renders LALRPOP `ParseError`s (plus our own lexer/semantic
//! errors) into readable messages with a source snippet, similar to what
//! rustc or clang would print.

use crate::lexer::LexError;
use crate::token::Tok;
use lalrpop_util::ParseError;
use std::fmt;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct LineCol {
    pub line: usize, // 1-based
    pub col: usize,  // 1-based
}

pub fn line_col(src: &str, byte_offset: usize) -> LineCol {
    let offset = byte_offset.min(src.len());
    let mut line = 1;
    let mut last_newline = 0usize;
    for (i, b) in src.as_bytes()[..offset].iter().enumerate() {
        if *b == b'\n' {
            line += 1;
            last_newline = i + 1;
        }
    }
    let col = src[last_newline..offset].chars().count() + 1;
    LineCol { line, col }
}

#[derive(Debug, Clone)]
pub struct Diagnostic {
    pub message: String,
    pub start: usize,
    pub end: usize,
}

impl Diagnostic {
    pub fn render(&self, filename: &str, src: &str) -> String {
        let lc = line_col(src, self.start);
        let line_text = src.lines().nth(lc.line.saturating_sub(1)).unwrap_or("");
        let caret_pad = " ".repeat(lc.col.saturating_sub(1));
        let width = (self.end.saturating_sub(self.start)).max(1);
        let caret = "^".repeat(width.min(line_text.len().saturating_sub(lc.col - 1).max(1)));
        format!(
            "error: {msg}\n  --> {file}:{line}:{col}\n   |\n{line:>3}| {text}\n   | {pad}{caret}\n",
            msg = self.message,
            file = filename,
            line = lc.line,
            col = lc.col,
            text = line_text,
            pad = caret_pad,
            caret = caret,
        )
    }
}

impl fmt::Display for Diagnostic {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.message)
    }
}

pub fn from_parse_error(err: ParseError<usize, Tok, LexError>) -> Diagnostic {
    match err {
        ParseError::InvalidToken { location } => Diagnostic {
            message: "invalid token".to_string(),
            start: location,
            end: location + 1,
        },
        ParseError::UnrecognizedEof { location, expected } => Diagnostic {
            message: format!(
                "unexpected end of file; expected one of: {}",
                fmt_expected(&expected)
            ),
            start: location,
            end: location + 1,
        },
        ParseError::UnrecognizedToken { token, expected } => {
            let (start, tok, end) = token;
            Diagnostic {
                message: format!(
                    "unexpected token `{}`; expected one of: {}",
                    tok,
                    fmt_expected(&expected)
                ),
                start,
                end,
            }
        }
        ParseError::ExtraToken { token } => {
            let (start, tok, end) = token;
            Diagnostic {
                message: format!("unexpected extra token `{}`", tok),
                start,
                end,
            }
        }
        ParseError::User { error } => from_lex_error(error),
    }
}

pub fn from_lex_error(error: LexError) -> Diagnostic {
    match error {
        LexError::UnexpectedChar(c, pos) => Diagnostic {
            message: format!("unexpected character '{}'", c),
            start: pos,
            end: pos + c.len_utf8(),
        },
        LexError::UnterminatedString(pos) => Diagnostic {
            message: "unterminated string literal".to_string(),
            start: pos,
            end: pos + 1,
        },
        LexError::UnterminatedBlockComment(pos) => Diagnostic {
            message: "unterminated block comment".to_string(),
            start: pos,
            end: pos + 2,
        },
        LexError::InvalidNumber(text, pos) => Diagnostic {
            message: format!("invalid numeric literal '{}'", text),
            start: pos,
            end: pos + text.len().max(1),
        },
        LexError::InvalidEscape(c, pos) => Diagnostic {
            message: format!("invalid escape sequence '\\{}'", c),
            start: pos,
            end: pos + 1,
        },
        LexError::InvalidAssignmentTarget(start, end) => Diagnostic {
            message: "invalid assignment target: only identifiers, indexing, \
                       and member access may appear left of `=`"
                .to_string(),
            start,
            end,
        },
    }
}

fn fmt_expected(expected: &[String]) -> String {
    if expected.is_empty() {
        "nothing".to_string()
    } else {
        expected.join(", ")
    }
}
