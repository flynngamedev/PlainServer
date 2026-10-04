//! Hand-written lexer for the PlainS `.pls` language.
//!
//! This produces a stream of `Result<(usize, Tok, usize), LexError>` items,
//! which is exactly the shape LALRPOP's "external tokenizer" protocol
//! expects (`Spanned<Token, Location, Error>` = `Result<(L, T, L), E>`).
//! `usize` locations are byte offsets into the source string.

use crate::token::{lookup_keyword, Tok};
use std::fmt;
use std::str::CharIndices;

#[derive(Debug, Clone, PartialEq)]
pub enum LexError {
    UnexpectedChar(char, usize),
    UnterminatedString(usize),
    UnterminatedBlockComment(usize),
    InvalidNumber(String, usize),
    InvalidEscape(char, usize),
    /// Raised by the parser (not the lexer proper) when the left-hand side
    /// of an assignment isn't a valid assignment target. Kept in the same
    /// error type so parser and lexer errors can be reported uniformly.
    InvalidAssignmentTarget(usize, usize),
}

impl fmt::Display for LexError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            LexError::UnexpectedChar(c, pos) => {
                write!(f, "unexpected character '{}' at byte offset {}", c, pos)
            }
            LexError::UnterminatedString(pos) => {
                write!(f, "unterminated string literal starting at byte offset {}", pos)
            }
            LexError::UnterminatedBlockComment(pos) => {
                write!(f, "unterminated block comment starting at byte offset {}", pos)
            }
            LexError::InvalidNumber(text, pos) => {
                write!(f, "invalid numeric literal '{}' at byte offset {}", text, pos)
            }
            LexError::InvalidEscape(c, pos) => {
                write!(f, "invalid escape sequence '\\{}' at byte offset {}", c, pos)
            }
            LexError::InvalidAssignmentTarget(start, end) => {
                write!(
                    f,
                    "invalid assignment target (bytes {}..{}); only identifiers, \
                     indexing (`a[i]`), and member access (`a.b`) may appear on \
                     the left-hand side of `=`",
                    start, end
                )
            }
        }
    }
}

impl std::error::Error for LexError {}

pub type Spanned<T, L, E> = Result<(L, T, L), E>;

pub struct Lexer<'input> {
    input: &'input str,
    chars: std::iter::Peekable<CharIndices<'input>>,
    len: usize,
}

impl<'input> Lexer<'input> {
    pub fn new(input: &'input str) -> Self {
        Lexer {
            input,
            chars: input.char_indices().peekable(),
            len: input.len(),
        }
    }

    fn peek_char(&mut self) -> Option<char> {
        self.chars.peek().map(|&(_, c)| c)
    }

    fn peek_at(&mut self, offset_from_current: usize) -> Option<char> {
        // Small lookahead helper used for things like `0x`, `1.5`, `1e3`.
        let pos = self.cur_pos();
        self.input[pos..].chars().nth(offset_from_current)
    }

    fn cur_pos(&mut self) -> usize {
        self.chars.peek().map(|&(i, _)| i).unwrap_or(self.len)
    }

    fn bump(&mut self) -> Option<(usize, char)> {
        self.chars.next()
    }

    /// Skip whitespace and comments. Comments never produce tokens or AST
    /// nodes anywhere in the pipeline -- they are discarded right here.
    fn skip_trivia(&mut self) -> Result<(), LexError> {
        loop {
            match self.peek_char() {
                Some(c) if c.is_whitespace() => {
                    self.bump();
                }
                Some('/') => {
                    if self.peek_at(1) == Some('/') {
                        // line comment
                        while let Some(c) = self.peek_char() {
                            if c == '\n' {
                                break;
                            }
                            self.bump();
                        }
                    } else if self.peek_at(1) == Some('*') {
                        let start = self.cur_pos();
                        self.bump(); // '/'
                        self.bump(); // '*'
                        let mut closed = false;
                        while let Some((_, c)) = self.bump() {
                            if c == '*' && self.peek_char() == Some('/') {
                                self.bump();
                                closed = true;
                                break;
                            }
                        }
                        if !closed {
                            return Err(LexError::UnterminatedBlockComment(start));
                        }
                    } else {
                        break;
                    }
                }
                _ => break,
            }
        }
        Ok(())
    }

    fn lex_number(&mut self, start: usize, first: char) -> Result<Tok, LexError> {
        // Hex literal: 0x... / 0X...
        // (`first` is the already-consumed leading digit; only the
        // following character needs a peek to detect the 0x/0b prefix.)
        if first == '0' && matches!(self.peek_char(), Some('x') | Some('X')) {
            self.bump(); // x/X
            let digits_start = self.cur_pos();
            while matches!(self.peek_char(), Some(c) if c.is_ascii_hexdigit()) {
                self.bump();
            }
            let end = self.cur_pos();
            let text = &self.input[digits_start..end];
            if text.is_empty() {
                return Err(LexError::InvalidNumber(self.input[start..end].to_string(), start));
            }
            let value = i64::from_str_radix(text, 16)
                .map_err(|_| LexError::InvalidNumber(self.input[start..end].to_string(), start))?;
            return Ok(Tok::Int(value));
        }

        // Binary literal: 0b... / 0B...
        if first == '0' && matches!(self.peek_char(), Some('b') | Some('B')) {
            self.bump(); // b/B
            let digits_start = self.cur_pos();
            while matches!(self.peek_char(), Some('0') | Some('1')) {
                self.bump();
            }
            let end = self.cur_pos();
            let text = &self.input[digits_start..end];
            if text.is_empty() {
                return Err(LexError::InvalidNumber(self.input[start..end].to_string(), start));
            }
            let value = i64::from_str_radix(text, 2)
                .map_err(|_| LexError::InvalidNumber(self.input[start..end].to_string(), start))?;
            return Ok(Tok::Int(value));
        }

        // Decimal integer / float / scientific notation.
        // (`first` -- the leading digit -- was already consumed by the
        // caller; the byte-offset slice below picks it back up via `start`.)
        while matches!(self.peek_char(), Some(c) if c.is_ascii_digit()) {
            self.bump();
        }

        let mut is_float = false;

        // Fractional part: only consume '.' if followed by a digit, so that
        // member access like `10.toString` (or, realistically, `arr[0].x`)
        // is never swallowed into a number.
        if self.peek_char() == Some('.') && matches!(self.peek_at(1), Some(c) if c.is_ascii_digit())
        {
            is_float = true;
            self.bump(); // '.'
            while matches!(self.peek_char(), Some(c) if c.is_ascii_digit()) {
                self.bump();
            }
        }

        // Exponent part: e/E [+/-] digits
        if matches!(self.peek_char(), Some('e') | Some('E')) {
            let mut lookahead = 1;
            if matches!(self.peek_at(1), Some('+') | Some('-')) {
                lookahead = 2;
            }
            if matches!(self.peek_at(lookahead), Some(c) if c.is_ascii_digit()) {
                is_float = true;
                self.bump(); // e/E
                if matches!(self.peek_char(), Some('+') | Some('-')) {
                    self.bump();
                }
                while matches!(self.peek_char(), Some(c) if c.is_ascii_digit()) {
                    self.bump();
                }
            }
        }

        let end = self.cur_pos();
        let text = &self.input[start..end];
        if is_float {
            let value: f64 = text
                .parse()
                .map_err(|_| LexError::InvalidNumber(text.to_string(), start))?;
            Ok(Tok::Float(value))
        } else {
            let value: i64 = text
                .parse()
                .map_err(|_| LexError::InvalidNumber(text.to_string(), start))?;
            Ok(Tok::Int(value))
        }
    }

    fn lex_string(&mut self, start: usize, quote: char) -> Result<Tok, LexError> {
        // NOTE: the opening quote was already consumed by the caller
        // (`next()`), which is where `quote` came from -- don't bump again.
        let mut s = String::new();
        loop {
            match self.bump() {
                None => return Err(LexError::UnterminatedString(start)),
                Some((pos, c)) if c == quote => {
                    let _ = pos;
                    break;
                }
                Some((_, '\n')) => return Err(LexError::UnterminatedString(start)),
                Some((pos, '\\')) => match self.bump() {
                    None => return Err(LexError::UnterminatedString(start)),
                    Some((_, esc)) => {
                        let resolved = match esc {
                            'n' => '\n',
                            't' => '\t',
                            'r' => '\r',
                            '0' => '\0',
                            '\\' => '\\',
                            '"' => '"',
                            '\'' => '\'',
                            other => return Err(LexError::InvalidEscape(other, pos)),
                        };
                        s.push(resolved);
                    }
                },
                Some((_, c)) => s.push(c),
            }
        }
        Ok(Tok::Str(s))
    }

    fn lex_ident(&mut self, start: usize) -> Tok {
        while matches!(self.peek_char(), Some(c) if c.is_ascii_alphanumeric() || c == '_') {
            self.bump();
        }
        let end = self.cur_pos();
        let text = &self.input[start..end];
        lookup_keyword(text).unwrap_or_else(|| Tok::Ident(text.to_string()))
    }
}

impl<'input> Iterator for Lexer<'input> {
    type Item = Spanned<Tok, usize, LexError>;

    fn next(&mut self) -> Option<Self::Item> {
        if let Err(e) = self.skip_trivia() {
            return Some(Err(e));
        }

        let (start, c) = self.bump()?;

        macro_rules! one {
            ($tok:expr) => {
                Some(Ok((start, $tok, start + c.len_utf8())))
            };
        }
        macro_rules! two {
            ($tok:expr) => {{
                let (_, c2) = self.bump().unwrap();
                Some(Ok((start, $tok, start + c.len_utf8() + c2.len_utf8())))
            }};
        }

        match c {
            '{' => one!(Tok::LBrace),
            '}' => one!(Tok::RBrace),
            '(' => one!(Tok::LParen),
            ')' => one!(Tok::RParen),
            '[' => one!(Tok::LBracket),
            ']' => one!(Tok::RBracket),
            ',' => one!(Tok::Comma),
            ';' => one!(Tok::Semi),
            '?' => one!(Tok::Question),
            '~' => one!(Tok::Tilde),

            ':' => {
                if self.peek_char() == Some(':') {
                    two!(Tok::ColonColon)
                } else {
                    one!(Tok::Colon)
                }
            }
            '.' => one!(Tok::Dot),

            '=' => {
                if self.peek_char() == Some('=') {
                    two!(Tok::EqEq)
                } else {
                    one!(Tok::Assign)
                }
            }
            '!' => {
                if self.peek_char() == Some('=') {
                    two!(Tok::NotEq)
                } else {
                    one!(Tok::Bang)
                }
            }
            '<' => {
                if self.peek_char() == Some('=') {
                    two!(Tok::LtEq)
                } else {
                    one!(Tok::Lt)
                }
            }
            '>' => {
                if self.peek_char() == Some('=') {
                    two!(Tok::GtEq)
                } else {
                    one!(Tok::Gt)
                }
            }
            '+' => {
                if self.peek_char() == Some('=') {
                    two!(Tok::PlusEq)
                } else {
                    one!(Tok::Plus)
                }
            }
            '-' => {
                if self.peek_char() == Some('=') {
                    two!(Tok::MinusEq)
                } else {
                    one!(Tok::Minus)
                }
            }
            '*' => {
                if self.peek_char() == Some('=') {
                    two!(Tok::StarEq)
                } else {
                    one!(Tok::Star)
                }
            }
            '/' => {
                // Comments are already stripped by skip_trivia, so a '/'
                // reaching here is always an operator.
                if self.peek_char() == Some('=') {
                    two!(Tok::SlashEq)
                } else {
                    one!(Tok::Slash)
                }
            }
            '%' => {
                if self.peek_char() == Some('=') {
                    two!(Tok::PercentEq)
                } else {
                    one!(Tok::Percent)
                }
            }
            '&' => {
                if self.peek_char() == Some('&') {
                    two!(Tok::AndAnd)
                } else {
                    Some(Err(LexError::UnexpectedChar('&', start)))
                }
            }
            '|' => {
                if self.peek_char() == Some('|') {
                    two!(Tok::OrOr)
                } else {
                    Some(Err(LexError::UnexpectedChar('|', start)))
                }
            }

            '"' | '\'' => {
                match self.lex_string(start, c) {
                    Ok(tok) => {
                        let end = self.cur_pos();
                        Some(Ok((start, tok, end)))
                    }
                    Err(e) => Some(Err(e)),
                }
            }

            c if c.is_ascii_digit() => match self.lex_number(start, c) {
                Ok(tok) => {
                    let end = self.cur_pos();
                    Some(Ok((start, tok, end)))
                }
                Err(e) => Some(Err(e)),
            },

            c if c.is_ascii_alphabetic() || c == '_' => {
                let tok = self.lex_ident(start);
                let end = self.cur_pos();
                Some(Ok((start, tok, end)))
            }

            other => Some(Err(LexError::UnexpectedChar(other, start))),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn toks(src: &str) -> Vec<Tok> {
        Lexer::new(src).map(|r| r.unwrap().1).collect()
    }

    #[test]
    fn numbers() {
        assert_eq!(toks("123"), vec![Tok::Int(123)]);
        assert_eq!(toks("3.14"), vec![Tok::Float(3.14)]);
        assert_eq!(toks("0xFF"), vec![Tok::Int(255)]);
        assert_eq!(toks("0b1010"), vec![Tok::Int(10)]);
        assert_eq!(toks("1.5e3"), vec![Tok::Float(1500.0)]);
        assert_eq!(toks("1.5e-3"), vec![Tok::Float(0.0015)]);
    }

    #[test]
    fn member_vs_float() {
        assert_eq!(
            toks("a.b"),
            vec![Tok::Ident("a".into()), Tok::Dot, Tok::Ident("b".into())]
        );
        assert_eq!(toks("3.14"), vec![Tok::Float(3.14)]);
    }

    #[test]
    fn scope_vs_colon() {
        assert_eq!(toks("PlS::Foo"), vec![Tok::Ident("PlS".into()), Tok::ColonColon, Tok::Ident("Foo".into())]);
        assert_eq!(toks("a ? b : c"), vec![
            Tok::Ident("a".into()), Tok::Question, Tok::Ident("b".into()), Tok::Colon, Tok::Ident("c".into())
        ]);
    }

    #[test]
    fn strings_with_escapes() {
        assert_eq!(toks(r#""hi\n""#), vec![Tok::Str("hi\n".to_string())]);
        assert_eq!(toks("'single'"), vec![Tok::Str("single".to_string())]);
    }

    #[test]
    fn comments_are_skipped() {
        assert_eq!(
            toks("1 // comment\n2 /* block \n comment */ 3"),
            vec![Tok::Int(1), Tok::Int(2), Tok::Int(3)]
        );
    }

    #[test]
    fn keywords_vs_idents() {
        assert_eq!(toks("if elif else while forgo"), vec![
            Tok::KwIf, Tok::KwElif, Tok::KwElse, Tok::KwWhile, Tok::Ident("forgo".into())
        ]);
    }

    #[test]
    fn in_is_a_keyword_but_only_on_its_own() {
        // `in` drives the for-in loop, so it must lex as a keyword -- while
        // identifiers that merely start with those letters must not.
        assert_eq!(toks("for x in xs"), vec![
            Tok::KwFor, Tok::Ident("x".into()), Tok::KwIn, Tok::Ident("xs".into())
        ]);
        assert_eq!(toks("input index int"), vec![
            Tok::Ident("input".into()), Tok::Ident("index".into()), Tok::Ident("int".into())
        ]);
    }
}
