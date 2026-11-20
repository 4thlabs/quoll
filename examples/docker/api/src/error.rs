use serde::{Deserialize, Serialize};

/// The body of Docker's errors, which are not RFC 9457 problems: `quoll::rest::Error::body_as` reads it.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct DockerError {
    pub message: String,
}
