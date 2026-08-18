// This build.rs exists purely so `cargo build` / `cargo build -r` / `cargo test`
// / `cargo clean` can drive the real C++ build/test for this project. There
// is no actual Rust code.
//
// Linux/macOS: uses the top-level Makefile (`make config=debug|release ...`).
// Windows: uses CMake directly, since there's no Makefile support there.
//
// The C++ build output is placed inside Cargo's own OUT_DIR (under target/)
// rather than a repo-root `build/` directory. `cargo clean` is a builtin
// that can't be hooked or aliased, but it does wipe `target/` unconditionally
// -- so by building inside OUT_DIR, `cargo clean` genuinely deletes the C++
// build artifacts too, for free. On Linux/macOS the Makefile's own `luau`/
// `luau-analyze` targets then symlink those OUT_DIR binaries to the repo
// root (`ln -fs`), so they stay easy to find; on Windows we do the same
// manually since CMake has no equivalent alias rule. Those symlinks go
// stale (dangling) after `cargo clean` until the next build recreates them.
//
// Compiling is slow and cargo hides a build script's stdout/stderr unless it
// fails, so we open the real terminal directly (see build-stub/progress.rs)
// to show a live "Compiling [n/total] File.cpp" line while it runs.
#[path = "rust-build-stub/progress.rs"]
mod progress;

use progress::{stream_and_report, Progress};
use std::env;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

fn run(cmd: &mut Command) {
    let status = cmd.status().unwrap_or_else(|e| {
        panic!("failed to run {:?}: {}", cmd, e);
    });
    if !status.success() {
        panic!("command {:?} failed with {}", cmd, status);
    }
}

/// Extracts the source file being compiled from a Makefile compile line like
/// `g++ VM/src/lvm.cpp -O2 ... -c -MMD -MP -o build/release/VM/src/lvm.cpp.o`.
fn extract_make_source(line: &str) -> Option<String> {
    if !line.contains("-MMD -MP -o") {
        return None;
    }
    line.split_whitespace()
        .find(|tok| (tok.ends_with(".cpp") || tok.ends_with(".c")) && !tok.starts_with('-'))
        .map(|s| s.to_string())
}

/// Extracts the source file from a `cmake --build` line like
/// `Building CXX object VM/CMakeFiles/Luau.VM.dir/src/lgc.cpp.o`.
fn extract_cmake_source(line: &str) -> Option<String> {
    if !line.starts_with("Building") {
        return None;
    }
    line.rsplit('/').next().map(|s| s.trim_end_matches(".o").to_string())
}

fn count_make_compile_steps(manifest_dir: &str, build_dir: &Path, jobs: &str) -> usize {
    let output = Command::new("make")
        .current_dir(manifest_dir)
        .arg("-n")
        .arg(format!("-j{jobs}"))
        .arg(format!("BUILD={}", build_dir.display()))
        .arg("luau")
        .arg("luau-analyze")
        .output();
    match output {
        Ok(out) => String::from_utf8_lossy(&out.stdout)
            .lines()
            .filter(|l| l.contains("-MMD -MP -o"))
            .count(),
        Err(_) => 0,
    }
}

/// (Re)creates `link` as a symlink pointing at `target`, best-effort.
fn refresh_symlink(link: &Path, target: &Path) {
    let _ = std::fs::remove_file(link);
    #[cfg(unix)]
    let _ = std::os::unix::fs::symlink(target, link);
    #[cfg(windows)]
    let _ = std::os::windows::fs::symlink_file(target, link);
}

/// Keeps `build/cc` configured with `CMAKE_EXPORT_COMPILE_COMMANDS=ON`, which
/// is what the repo-root `compile_commands.json` symlink points into (for
/// clangd). This is a separate, tiny CMake tree used only to generate that
/// file -- it's never built, and it doesn't affect the real make/cmake build
/// above. Reconfiguring is a cheap no-op once CMakeCache.txt is up to date,
/// so it's fine to just always do it.
fn configure_compile_commands(manifest_dir: &str) {
    let build_cc = Path::new(manifest_dir).join("build").join("cc");
    let result = Command::new("cmake")
        .arg("-S")
        .arg(manifest_dir)
        .arg("-B")
        .arg(&build_cc)
        .arg("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON")
        .arg("-DCMAKE_BUILD_TYPE=Debug")
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status();
    match result {
        Ok(s) if s.success() => {}
        Ok(_) => eprintln!("warning: cmake configure of build/cc (for compile_commands.json) failed"),
        Err(_) => {
            eprintln!("warning: cmake not found; skipping build/cc setup for compile_commands.json/clangd")
        }
    }
}

fn main() {
    // Deliberately no cargo:rerun-if-changed directives: this build script's
    // whole job is to delegate to make/cmake's own incremental build, so it
    // needs to run on every `cargo build`/`cargo test`, not just when
    // CMakeLists.txt or the Makefile changes. Emitting even one
    // rerun-if-changed line switches cargo into fine-grained tracking mode
    // and it would stop rerunning build.rs (and therefore stop rebuilding)
    // when a .cpp/.h file changes. Omitting it keeps cargo's default:
    // rerun whenever anything in the package changes.

    let profile = env::var("PROFILE").unwrap_or_else(|_| "debug".into());
    let is_release = profile == "release";
    let manifest_dir = env::var("CARGO_MANIFEST_DIR").unwrap();
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());
    let jobs = env::var("NUM_JOBS").unwrap_or_else(|_| "8".into());

    configure_compile_commands(&manifest_dir);

    if cfg!(target_os = "windows") {
        let build_type = if is_release { "RelWithDebInfo" } else { "Debug" };
        let build_dir = out_dir.join("cmake");
        std::fs::create_dir_all(&build_dir).unwrap();

        run(Command::new("cmake")
            .arg("-S")
            .arg(&manifest_dir)
            .arg("-B")
            .arg(&build_dir)
            .arg(format!("-DCMAKE_BUILD_TYPE={build_type}")));

        let mut child = Command::new("cmake")
            .arg("--build")
            .arg(&build_dir)
            .arg("--config")
            .arg(build_type)
            .arg("--target")
            .arg("Luau.Repl.CLI")
            .arg("--target")
            .arg("Luau.Analyze.CLI")
            .arg("-j")
            .arg(&jobs)
            .stdout(Stdio::piped())
            .spawn()
            .expect("failed to invoke cmake");

        let mut progress = Progress::new("Compiling", 0);
        stream_and_report(&mut child, &mut progress, extract_cmake_source);
        let status = child.wait().expect("failed to wait on cmake");
        progress.finish(if status.success() { "Build finished" } else { "Build failed" });
        assert!(status.success(), "cmake build failed");

        let out_bin_dir = build_dir.join(build_type);
        let root = Path::new(&manifest_dir);
        refresh_symlink(&root.join("luau.exe"), &out_bin_dir.join("Luau.Repl.CLI.exe"));
        refresh_symlink(&root.join("luau-analyze.exe"), &out_bin_dir.join("Luau.Analyze.CLI.exe"));
        eprintln!("\nLuau CLI binaries: {}\\luau.exe, {0}\\luau-analyze.exe\n", root.display());
    } else {
        let build_dir = out_dir.join("build");
        let total = count_make_compile_steps(&manifest_dir, &build_dir, &jobs);

        let mut child = Command::new("make")
            .current_dir(&manifest_dir)
            .arg(format!("-j{jobs}"))
            .arg(format!("BUILD={}", build_dir.display()))
            .arg("luau")
            .arg("luau-analyze")
            .stdout(Stdio::piped())
            .spawn()
            .expect("failed to invoke make");

        let mut progress = Progress::new("Compiling", total);
        stream_and_report(&mut child, &mut progress, extract_make_source);
        let status = child.wait().expect("failed to wait on make");
        progress.finish(if status.success() { "Build finished" } else { "Build failed" });
        assert!(status.success(), "make build failed");

        // `make ... luau luau-analyze` already symlinks these to the repo
        // root itself (see the Makefile's `luau`/`luau-analyze` targets).
        eprintln!("\nLuau CLI binaries: {}/luau, {0}/luau-analyze\n", manifest_dir);
    }
}
