//! `plscc` -- the PlainS native build orchestrator. This is what
//! `pls build` / `pls run` shell out to (see pls.py). Not meant to be a
//! pleasant hand-typed CLI; pls.py is the friendly front door.
//!
//! Layout it manages inside a project directory:
//!   build/CMakeLists.txt       (regenerated every build)
//!   build/generated/main.cpp   (regenerated every build)
//!   build/_cmake/              (CMake's own cache/object files)
//!   build/bin/<name>           (final native executable)
//!
//! Compared with PlainVulkan's pvcc this is a much smaller job: a server
//! links against sockets and a thread library and nothing else -- no Vulkan
//! loader, no GLFW, no shader compilation step -- so the generated
//! CMakeLists has no find_package calls that can fail on a headless box.
//! That is deliberate: a game server should build and run on a machine with
//! no GPU and no display.

use pls_compiler::codegen;
use pls_compiler::project::PlsProject;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};

fn usage() -> ! {
    eprintln!("Usage:");
    eprintln!("  plscc build <project_dir> --runtime-dir <dir> [--release]");
    eprintln!("  plscc run   <project_dir> --runtime-dir <dir> [--release] [-- <args...>]");
    eprintln!("  plscc check <file.pls>");
    eprintln!("  plscc emit  <file.pls>     (print generated C++ to stdout)");
    std::process::exit(2);
}

struct Args {
    project_dir: PathBuf,
    runtime_dir: PathBuf,
    release: bool,
    run_args: Vec<String>,
}

fn parse_build_args(rest: &[String]) -> Args {
    if rest.is_empty() {
        usage();
    }
    let project_dir = PathBuf::from(&rest[0]);
    let mut runtime_dir = None;
    let mut release = false;
    let mut run_args = Vec::new();
    let mut i = 1;
    while i < rest.len() {
        match rest[i].as_str() {
            "--runtime-dir" => {
                i += 1;
                runtime_dir = rest.get(i).map(PathBuf::from);
            }
            "--release" => release = true,
            "--" => {
                run_args = rest[i + 1..].to_vec();
                break;
            }
            other => {
                eprintln!("error: unrecognized argument '{}'", other);
                usage();
            }
        }
        i += 1;
    }
    let runtime_dir = match runtime_dir {
        Some(d) => d,
        None => {
            eprintln!("error: --runtime-dir is required");
            usage();
        }
    };
    Args { project_dir, runtime_dir, release, run_args }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 {
        usage();
    }
    match args[1].as_str() {
        "check" => cmd_check(&args[2..]),
        "emit" => cmd_emit(&args[2..]),
        "build" => match cmd_build(&parse_build_args(&args[2..])) {
            Ok(_bin_path) => ExitCode::SUCCESS,
            Err(e) => {
                e.report();
                ExitCode::FAILURE
            }
        },
        "run" => {
            let a = parse_build_args(&args[2..]);
            match cmd_build(&a) {
                Ok(bin_path) => {
                    println!("--- running {} ---", bin_path.display());
                    let status = Command::new(&bin_path).args(&a.run_args).status();
                    match status {
                        Ok(s) if s.success() => ExitCode::SUCCESS,
                        Ok(s) => {
                            eprintln!("server exited with status {}", s);
                            ExitCode::FAILURE
                        }
                        Err(e) => {
                            eprintln!("error: failed to launch '{}': {}", bin_path.display(), e);
                            ExitCode::FAILURE
                        }
                    }
                }
                Err(e) => {
                    e.report();
                    ExitCode::FAILURE
                }
            }
        }
        _ => usage(),
    }
}

fn cmd_check(rest: &[String]) -> ExitCode {
    if rest.is_empty() {
        usage();
    }
    let path = Path::new(&rest[0]);
    let src = match fs::read_to_string(path) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("error: could not read '{}': {}", path.display(), e);
            return ExitCode::FAILURE;
        }
    };
    match pls_transpiler::parse(&src) {
        Ok(program) => {
            // `check` runs codegen too, discarding the C++. Parsing alone
            // would miss everything codegen diagnoses -- wrong argument
            // counts, assignment to const, break outside a loop -- which is
            // most of what a script actually gets wrong.
            match codegen::generate(&program) {
                Ok(gen) => {
                    if !gen.warnings.is_empty() {
                        eprint!("{}", render_codegen_diags(path, &src, &gen.warnings));
                    }
                    println!("OK: {} parses and compiles cleanly", path.display());
                    ExitCode::SUCCESS
                }
                Err(diags) => {
                    eprint!("{}", render_codegen_diags(path, &src, &diags));
                    ExitCode::FAILURE
                }
            }
        }
        Err(diags) => {
            eprint!(
                "{}",
                pls_transpiler::render_diagnostics(&path.display().to_string(), &src, &diags)
            );
            ExitCode::FAILURE
        }
    }
}

/// Prints the generated C++ without building it. Useful when a script does
/// something surprising and you want to see what it actually compiled to.
fn cmd_emit(rest: &[String]) -> ExitCode {
    if rest.is_empty() {
        usage();
    }
    let path = Path::new(&rest[0]);
    let src = match fs::read_to_string(path) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("error: could not read '{}': {}", path.display(), e);
            return ExitCode::FAILURE;
        }
    };
    let program = match pls_transpiler::parse(&src) {
        Ok(p) => p,
        Err(diags) => {
            eprint!(
                "{}",
                pls_transpiler::render_diagnostics(&path.display().to_string(), &src, &diags)
            );
            return ExitCode::FAILURE;
        }
    };
    match codegen::generate(&program) {
        Ok(gen) => {
            if !gen.warnings.is_empty() {
                eprint!("{}", render_codegen_diags(path, &src, &gen.warnings));
            }
            print!("{}", gen.cpp);
            ExitCode::SUCCESS
        }
        Err(diags) => {
            eprint!("{}", render_codegen_diags(path, &src, &diags));
            ExitCode::FAILURE
        }
    }
}

/// Runs the full parse -> codegen -> CMake build pipeline. Returns the path
/// to the resulting native executable on success.
/// Why a build failed. The distinction matters only for formatting: a
/// `Diagnostics` payload is an already-rendered, multi-line source excerpt
/// that carries its own "error:" markers and trailing newline, so prefixing it
/// again produced the doubled `error: error:` that used to head every parse
/// and codegen failure.
enum BuildError {
    /// Pre-rendered diagnostics from the parser or the code generator.
    Diagnostics(String),
    /// A one-line failure (missing file, CMake exited non-zero, ...).
    Message(String),
}

impl From<String> for BuildError {
    fn from(s: String) -> Self {
        BuildError::Message(s)
    }
}

impl BuildError {
    fn report(&self) {
        match self {
            BuildError::Diagnostics(text) => eprint!("{}", text),
            BuildError::Message(msg) => eprintln!("error: {}", msg),
        }
    }
}

fn cmd_build(args: &Args) -> Result<PathBuf, BuildError> {
    let project_file = args.project_dir.join("plsproject.json");
    let project = PlsProject::load(&project_file)?;

    let entry_path = args.project_dir.join(&project.entry);
    let src = fs::read_to_string(&entry_path)
        .map_err(|e| format!("could not read entry file '{}': {}", entry_path.display(), e))?;

    println!("[1/4] parsing {}", entry_path.display());
    let program = pls_transpiler::parse(&src).map_err(|diags| {
        BuildError::Diagnostics(pls_transpiler::render_diagnostics(
            &entry_path.display().to_string(),
            &src,
            &diags,
        ))
    })?;

    println!("[2/4] generating C++");
    let gen = codegen::generate(&program)
        .map_err(|diags| BuildError::Diagnostics(render_codegen_diags(&entry_path, &src, &diags)))?;
    if !gen.warnings.is_empty() {
        eprint!("{}", render_codegen_diags(&entry_path, &src, &gen.warnings));
    }

    let build_dir = args.project_dir.join("build");
    let generated_dir = build_dir.join("generated");
    fs::create_dir_all(&generated_dir).map_err(|e| e.to_string())?;
    fs::write(generated_dir.join("main.cpp"), &gen.cpp).map_err(|e| e.to_string())?;

    // dunce::canonicalize behaves like fs::canonicalize but strips the
    // Windows verbatim \\?\ prefix when it isn't required. Plain
    // fs::canonicalize returns e.g. \\?\C:\...\runtime, which after the
    // backslash->slash replacement below becomes //?/C:/...  -- CMake
    // tolerates that during configure, but MSBuild/cl.exe then can't open
    // the source files under it (the \\?\ long-path escape is only valid
    // with backslashes), producing C1083 "cannot open source file" for
    // every runtime .cpp.
    let runtime_dir_abs = dunce::canonicalize(&args.runtime_dir)
        .map_err(|e| format!("--runtime-dir '{}' not found: {}", args.runtime_dir.display(), e))?;

    let cmake_lists = format!(
        r#"cmake_minimum_required(VERSION 3.16)
project({name} CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE {build_type})
endif()
# Defaults from plsproject.json. These MUST be set before add_subdirectory:
# the code that reads them lives in the runtime library, not in the generated
# main.cpp, so runtime/CMakeLists.txt is what turns them into compile
# definitions (on pls_runtime, PUBLIC, so both halves agree).
#
# They were previously applied with target_compile_definitions on the game
# target only, which meant the runtime never saw them and every setting in
# plsproject.json was silently ignored.
#
# What each one does:
#   PLS_DEFAULT_TICKRATE -- the tick rate a script gets if it never calls
#                           PlS::SetTickrate.
#   PLS_MAX_PLAYERS      -- the hard ceiling on simultaneous connections,
#                           enforced in the accept loop.
# `server.port` deliberately has no compile-time effect: PlS::Listen(port)
# always takes an explicit port, so there is nothing for a default to feed.
set(PLS_DEFAULT_TICKRATE {tickrate})
set(PLS_MAX_PLAYERS {max_players})

add_subdirectory("{runtime_dir}" pls_runtime_build)
add_executable({name} generated/main.cpp)
target_link_libraries({name} PRIVATE pls_runtime)
set_target_properties({name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${{CMAKE_CURRENT_SOURCE_DIR}}/bin")

# Multi-config generators (Visual Studio, Xcode) append a per-config
# subdirectory to the output path unless it's overridden per config, which
# is why the loop below exists -- without it `pls run` looks for the binary
# in bin/ while MSBuild wrote it to bin/Release/.
foreach(CFG ${{CMAKE_CONFIGURATION_TYPES}})
  string(TOUPPER ${{CFG}} CFG_UPPER)
  set_target_properties({name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_${{CFG_UPPER}} "${{CMAKE_CURRENT_SOURCE_DIR}}/bin")
endforeach()
"#,
        name = sanitize_target_name(&project.name),
        build_type = if args.release { "Release" } else { "RelWithDebInfo" },
        runtime_dir = runtime_dir_abs.display().to_string().replace('\\', "/"),
        tickrate = project.server.tickrate,
        max_players = project.server.max_players,
    );
    fs::write(build_dir.join("CMakeLists.txt"), cmake_lists).map_err(|e| e.to_string())?;

    let cmake_bin_dir = build_dir.join("_cmake");
    fs::create_dir_all(&cmake_bin_dir).map_err(|e| e.to_string())?;

    println!("[3/4] configuring CMake");
    run_checked(
        Command::new("cmake")
            .arg("-S")
            .arg(&build_dir)
            .arg("-B")
            .arg(&cmake_bin_dir)
            .arg(format!(
                "-DCMAKE_BUILD_TYPE={}",
                if args.release { "Release" } else { "RelWithDebInfo" }
            )),
    )?;

    println!("[4/4] compiling native binary");
    let config = if args.release { "Release" } else { "RelWithDebInfo" };
    run_checked(
        Command::new("cmake")
            .arg("--build")
            .arg(&cmake_bin_dir)
            .arg("--config")
            .arg(config)
            .arg("-j")
            .arg(num_cpus_guess().to_string()),
    )?;

    let bin_name = sanitize_target_name(&project.name);
    let exe_name =
        if cfg!(windows) { format!("{}.exe", bin_name) } else { bin_name.clone() };
    let bin_dir = build_dir.join("bin");
    let mut bin_path = bin_dir.join(&exe_name);
    if !bin_path.exists() {
        for cfg in ["Release", "RelWithDebInfo", "Debug"] {
            let candidate = bin_dir.join(cfg).join(&exe_name);
            if candidate.exists() {
                bin_path = candidate;
                break;
            }
        }
    }
    if !bin_path.exists() {
        return Err(BuildError::Message(format!(
            "build reported success but expected binary not found at '{}'",
            bin_path.display()
        )));
    }
    println!("built: {}", bin_path.display());
    Ok(bin_path)
}

fn sanitize_target_name(name: &str) -> String {
    let mut out = String::new();
    for c in name.chars() {
        if c.is_ascii_alphanumeric() || c == '_' {
            out.push(c);
        } else {
            out.push('_');
        }
    }
    if out.is_empty() || out.chars().next().unwrap().is_ascii_digit() {
        out.insert_str(0, "pls_");
    }
    out
}

fn num_cpus_guess() -> usize {
    std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4)
}

fn run_checked(cmd: &mut Command) -> Result<(), String> {
    let status = cmd.status().map_err(|e| format!("failed to launch {:?}: {}", cmd, e))?;
    if status.success() {
        Ok(())
    } else {
        Err(format!("command {:?} failed with {}", cmd, status))
    }
}

fn render_codegen_diags(path: &Path, src: &str, diags: &[codegen::CodegenDiagnostic]) -> String {
    let mut out = String::new();
    for d in diags {
        let (start, end) = d.span.unwrap_or((0, 0));
        let pls_diag =
            pls_transpiler::diagnostics::Diagnostic { message: d.message.clone(), start, end };
        out.push_str(&pls_diag.render(&path.display().to_string(), src));
    }
    out
}
