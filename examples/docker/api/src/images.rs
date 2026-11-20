use serde::{Deserialize, Serialize};

/// A line of a pull's progress. Docker answers 200 before it knows whether the pull succeeds: a failure is a line
/// with an `error`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PullProgress {
    #[serde(default)]
    pub status: Option<String>,
    #[serde(default)]
    pub error: Option<String>,
}
