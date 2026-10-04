//! `plsc` -- parser front-end for PlainS. Parses a `.pls` file and either
//! reports diagnostics or dumps the AST as JSON (`--ast`), which is what the
//! parser tests and any external tooling consume.

use std::fs;
use std::process::ExitCode;

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 {
        eprintln!("Usage: plsc <file.pls> [--ast]");
        return ExitCode::from(2);
    }
    let path = &args[1];
    let dump_ast = args.iter().any(|a| a == "--ast");

    let src = match fs::read_to_string(path) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("error: could not read '{}': {}", path, e);
            return ExitCode::FAILURE;
        }
    };

    match pls_transpiler::parse(&src) {
        Ok(program) => {
            if dump_ast {
                match serde_json::to_string_pretty(&program) {
                    Ok(j) => println!("{}", j),
                    Err(e) => {
                        eprintln!("error: could not serialize AST: {}", e);
                        return ExitCode::FAILURE;
                    }
                }
            } else {
                println!("OK: {} parses cleanly", path);
            }
            ExitCode::SUCCESS
        }
        Err(diags) => {
            eprint!("{}", pls_transpiler::render_diagnostics(path, &src, &diags));
            ExitCode::FAILURE
        }
    }
}
