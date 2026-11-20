//! A command of the tests: appends its arguments to `record.log` in its directory, or fails when its first is `fail`.

use std::fs::OpenOptions;
use std::io::Write;
use std::process::ExitCode;

fn main() -> ExitCode {
    let words: Vec<String> = std::env::args().skip(1).collect();

    if words.first().is_some_and(|word| word == "fail") {
        eprintln!("record: failing, as asked");
        return ExitCode::FAILURE;
    }

    let mut log = OpenOptions::new()
        .create(true)
        .append(true)
        .open("record.log")
        .expect("record.log opens in the current directory");
    writeln!(log, "{}", words.join(" ")).expect("record.log is written");

    ExitCode::SUCCESS
}
