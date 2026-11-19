//! What only the client reads in an error: the body of another API's answer as a type of the caller's.

use serde::de::DeserializeOwned;

use super::Error;

impl Error {
    /// The body another API answered with an error status, read as the JSON of `T`: the error shape of an API that
    /// does not answer RFC 9457 problems, or the extension members of one that does.
    ///
    /// `None` for an error that is not such an answer, or a body that is not the JSON of `T`, as a body longer than
    /// the 64 KiB the client keeps.
    pub fn body_as<T: DeserializeOwned>(&self) -> Option<T> {
        serde_json::from_slice(self.body()?).ok()
    }
}
