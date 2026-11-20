//! What the client shows of a URL in its errors and its spans: its origin and its path, without its credentials, its
//! query, where a query scheme sends its key, and its fragment.

use std::error::Error as StdError;

use reqwest::Url;
use reqwest_retry::RetryError;

/// `url` without its credentials, its query and its fragment: `http://localhost:8080/api/notes`.
pub(crate) fn redacted(url: &Url) -> String {
    format!("{}{}", url.origin().ascii_serialization(), url.path())
}

/// Takes the credentials, the query and the fragment out of the URL of each reqwest error `error` holds, which reqwest
/// writes in the error's message and its debug form. It reaches the errors of reqwest, of reqwest-middleware and of
/// reqwest-retry, as the client and its middleware give them: `error` itself, a reqwest error or a
/// `reqwest_middleware::Error`, and in that error's `anyhow` a reqwest error, a `reqwest_middleware::Error` or a
/// `RetryError`, as `retry` and a middleware's `reqwest_middleware::Error::middleware` wrap them. The error stays
/// reqwest's, which a caller still asks `is_timeout` or `is_connect`, and its URL still says where the call went.
///
/// A reqwest error inside an error type of the application's own is out of reach, a source behind `&`: the
/// application strips it itself, with `reqwest::Error::without_url`.
pub(crate) fn strip_urls(error: &mut (dyn StdError + Send + Sync + 'static)) {
    if let Some(error) = error.downcast_mut::<reqwest::Error>() {
        strip(error.url_mut());
    } else if let Some(error) = error.downcast_mut::<reqwest_middleware::Error>() {
        strip_middleware(error);
    }
}

/// [`strip_urls`] for the error of a middleware chain, which keeps a middleware's own in `anyhow`, reached by its type.
fn strip_middleware(error: &mut reqwest_middleware::Error) {
    let error = match error {
        reqwest_middleware::Error::Reqwest(error) => return strip(error.url_mut()),
        reqwest_middleware::Error::Middleware(error) => error,
    };

    if let Some(error) = error.downcast_mut::<reqwest::Error>() {
        strip(error.url_mut());
    } else if let Some(error) = error.downcast_mut::<reqwest_middleware::Error>() {
        strip_middleware(error);
    } else if let Some(RetryError::Error(error) | RetryError::WithRetries { err: error, .. }) =
        error.downcast_mut::<RetryError>()
    {
        strip_middleware(error);
    }
}

/// Takes the credentials, the query and the fragment out of `url`.
fn strip(url: Option<&mut Url>) {
    let Some(url) = url else {
        return;
    };

    // Each fails only on a URL that cannot hold credentials, `mailto:`, which then has none to take out.
    let _ = url.set_username("");
    let _ = url.set_password(None);
    url.set_query(None);
    url.set_fragment(None);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_stripped_url_keeps_its_origin_and_its_path() {
        let mut url =
            Url::parse("http://ana:secret@localhost:8080/api/keyed?token=k-1#top").expect("a url");

        strip(Some(&mut url));

        assert_eq!(url.as_str(), "http://localhost:8080/api/keyed");
        assert_eq!(redacted(&url), url.as_str());
    }
}
