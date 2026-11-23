//! What only the client reads in an error: the body of another API's answer as a type of the caller's; and the URL
//! of a reqwest error, which an error keeps without its query.

use std::error::Error as StdError;

use serde::de::DeserializeOwned;

use super::Error;
use crate::__private::client::strip_urls;

impl<E> Error<E> {
    /// The body another API answered with an error status, read as the JSON of `T`: the extension members of a
    /// problem, or a shape the api's own, [`detail`](Error::detail), does not read.
    ///
    /// `None` for an error that is not such an answer, or a body that is not the JSON of `T`, as a body longer than
    /// the 64 KiB the client keeps.
    pub fn body_as<T: DeserializeOwned>(&self) -> Option<T> {
        serde_json::from_slice(self.body()?).ok()
    }
}

/// `error`, the URL of each reqwest error it holds without its credentials, its query and its fragment.
pub(super) fn without_query(
    mut error: Box<dyn StdError + Send + Sync>,
) -> Box<dyn StdError + Send + Sync> {
    strip_urls(&mut *error);
    error
}
