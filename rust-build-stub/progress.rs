// Cargo captures a build script's own stdout/stderr and only shows it on
// failure (or with `-vv`), so a normal `println!` in build.rs is invisible
// while the C++ side is compiling. To get a live progress line anyway, we
// open the real terminal device directly and write to that, bypassing
// cargo's capture entirely.
use std::fs::{File, OpenOptions};
use std::io::{BufRead, BufReader, Write};
use std::process::Child;

pub fn open_tty() -> Option<File> {
    #[cfg(unix)]
    {
        OpenOptions::new().write(true).open("/dev/tty").ok()
    }
    #[cfg(windows)]
    {
        OpenOptions::new().write(true).open("CONOUT$").ok()
    }
}

pub struct Progress {
    tty: Option<File>,
    label: &'static str,
    done: usize,
    total: usize,
}

impl Progress {
    pub fn new(label: &'static str, total: usize) -> Self {
        Progress { tty: open_tty(), label, done: 0, total }
    }

    fn write_line(&mut self, line: &str) {
        if let Some(tty) = &mut self.tty {
            // \x1b[2K clears the line, \r returns to column 0.
            let _ = write!(tty, "\r\x1b[2K{line}");
            let _ = tty.flush();
        }
    }

    pub fn step(&mut self, what: &str) {
        self.done += 1;
        let line = if self.total > 0 {
            format!("{} [{}/{}] {}", self.label, self.done, self.total, what)
        } else {
            format!("{} [{}] {}", self.label, self.done, what)
        };
        self.write_line(&line);
    }

    pub fn finish(&mut self, message: &str) {
        self.write_line(message);
        if let Some(tty) = &mut self.tty {
            let _ = writeln!(tty);
        }
    }
}

/// Reads `child`'s stdout line by line, calling `extract` on each line to
/// pull out a short human-readable description (e.g. the source file being
/// compiled); every line that yields `Some(..)` advances the progress bar.
/// Lines that don't match anything are ignored (they're usually just
/// linker/archiver noise).
pub fn stream_and_report(
    child: &mut Child,
    progress: &mut Progress,
    extract: impl Fn(&str) -> Option<String>,
) {
    let Some(stdout) = child.stdout.take() else { return };
    for line in BufReader::new(stdout).lines().map_while(Result::ok) {
        if let Some(what) = extract(&line) {
            progress.step(&what);
        }
    }
}
