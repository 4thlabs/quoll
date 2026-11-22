use std::fmt;

use serde::{Deserialize, Serialize};

/// The body of Docker's errors, which are not RFC 9457 problems: the api's `#[error(DockerError)]`, which each call's
/// error reads.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct DockerError {
    pub message: String,
}

/// Docker's message, which the error of a call says after its status: `another api answered 404: No such container`.
impl fmt::Display for DockerError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}
