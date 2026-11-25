use serde::{Deserialize, Serialize};

/// What `POST /containers/{id}/exec` runs in a container: the command, and the streams it is given.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct NewExec {
    pub cmd: Vec<String>,
    pub attach_stdin: bool,
    pub attach_stdout: bool,
    pub attach_stderr: bool,
    /// Whether the command runs in a TTY: its output is then sent as it is, in no frame, its errors with it.
    pub tty: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct CreatedExec {
    pub id: String,
}

/// A command as `GET /exec/{id}/json` shows it: whether it runs, and the code it exited with once it has.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct ExecInspect {
    pub running: bool,
    /// `None` until the command has ended.
    pub exit_code: Option<i64>,
}

/// How `POST /exec/{id}/start` runs it: attached, its streams on the connection, and with a TTY when it was made
/// with one.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct ExecStart {
    pub detach: bool,
    pub tty: bool,
}
