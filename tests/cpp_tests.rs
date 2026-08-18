// `cargo test` entry point for the real C++ test suite.
//
// Linux/macOS: `make test flags=true` (builds luau-tests, then runs it with
// --fflags=true, same as `make test flags=true` on the command line).
// Windows: builds Luau.UnitTest and Luau.Conformance via CMake, then runs
// each with --fflags=true.
//
// Uses the same OUT_DIR-backed build directory as build.rs (see the comment
// there on why: it's what makes `cargo clean` actually clean the C++ side
// too). Test binaries don't get OUT_DIR as a runtime env var the way build
// scripts do, so it's read here via the `env!` compile-time macro instead.
#[path = "../rust-build-stub/progress.rs"]
mod progress;

use progress::{stream_and_report, Progress};
use std::env;
use std::path::PathBuf;
use std::process::{Command, Stdio};

fn manifest_dir() -> String {
    env::var("CARGO_MANIFEST_DIR").unwrap()
}

fn out_dir() -> PathBuf {
    PathBuf::from(env!("OUT_DIR"))
}

fn is_release() -> bool {
    env::var("PROFILE").map(|p| p == "release").unwrap_or(false)
}

fn extract_make_source(line: &str) -> Option<String> {
    if !line.contains("-MMD -MP -o") {
        return None;
    }
    line.split_whitespace()
        .find(|tok| (tok.ends_with(".cpp") || tok.ends_with(".c")) && !tok.starts_with('-'))
        .map(|s| s.to_string())
}

fn extract_cmake_source(line: &str) -> Option<String> {
    if !line.starts_with("Building") {
        return None;
    }
    line.rsplit('/').next().map(|s| s.trim_end_matches(".o").to_string())
}

#[test]
fn cpp_test_suite() {
    let dir = manifest_dir();

    if cfg!(target_os = "windows") {
        let build_type = if is_release() { "RelWithDebInfo" } else { "Debug" };
        let build_dir = out_dir().join("cmake");

        let mut child = Command::new("cmake")
            .arg("--build")
            .arg(&build_dir)
            .arg("--config")
            .arg(build_type)
            .arg("--target")
            .arg("Luau.UnitTest")
            .arg("--target")
            .arg("Luau.Conformance")
            .stdout(Stdio::piped())
            .spawn()
            .expect("failed to invoke cmake");

        let mut progress = Progress::new("Compiling tests", 0);
        stream_and_report(&mut child, &mut progress, extract_cmake_source);
        let status = child.wait().expect("failed to wait on cmake");
        progress.finish(if status.success() { "Test build finished" } else { "Test build failed" });
        assert!(status.success(), "cmake build of test targets failed");

        for name in ["Luau.UnitTest", "Luau.Conformance"] {
            let exe = build_dir.join(build_type).join(format!("{name}.exe"));
            let status = Command::new(&exe)
                .arg("--fflags=true")
                .status()
                .unwrap_or_else(|e| panic!("failed to run {}: {}", exe.display(), e));
            assert!(status.success(), "{} failed", name);
        }
    } else {
        let build_dir = out_dir().join("build");
        let jobs = env::var("NUM_JOBS").unwrap_or_else(|_| "8".into());

        // `make test` both builds luau-tests and then runs it; we only want
        // a progress bar for the build half, so build first, then run.
        let mut build = Command::new("make")
            .current_dir(&dir)
            .arg(format!("-j{jobs}"))
            .arg(format!("BUILD={}", build_dir.display()))
            .arg("luau-tests")
            .stdout(Stdio::piped())
            .spawn()
            .expect("failed to invoke make");

        let mut progress = Progress::new("Compiling tests", 0);
        stream_and_report(&mut build, &mut progress, extract_make_source);
        let build_status = build.wait().expect("failed to wait on make");
        progress.finish(if build_status.success() { "Test build finished" } else { "Test build failed" });
        assert!(build_status.success(), "make luau-tests failed");

        let status = Command::new("make")
            .current_dir(&dir)
            .arg(format!("BUILD={}", build_dir.display()))
            .arg("test")
            .arg("flags=true")
            .status()
            .expect("failed to invoke make test");
        assert!(status.success(), "make test flags=true failed");
    }
}
