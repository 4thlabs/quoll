//! `request_id()`: the id of each request, kept from the caller or made, and sent back.

use std::convert::Infallible;
use std::fmt;
use std::hash::{BuildHasher, RandomState};
use std::sync::LazyLock;
use std::sync::atomic::{AtomicU64, Ordering};
use std::task::{Context, Poll};

use axum::extract::Request;
use axum::response::{IntoResponse, Response};
use http::{HeaderMap, HeaderName, HeaderValue};
use tower_layer::Layer;
use tower_service::Service;

use super::layer::ResponseFuture;
use super::trace;

/// The header a request id travels in, both ways.
const X_REQUEST_ID: HeaderName = HeaderName::from_static("x-request-id");

/// The longest request id kept from a caller, in bytes.
const MAX_LEN: usize = 128;

tokio::task_local! {
    /// The id of the request whose handler runs.
    static CURRENT: RequestId;
}

/// The id of a request, which [`request_id`] keeps from the caller's `X-Request-Id` or makes.
///
/// It is printable ASCII, so that a log shows it as it is. A handler reads the id of its request with
/// [`RequestId::current`], to pass it on to the APIs it calls.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct RequestId(HeaderValue);

impl RequestId {
    /// The id of the request whose handler runs, while its future runs; `None` outside a handler, in a task it
    /// spawned, in the stream an `Events`, a `JsonLines` or a `ByteStream` sends, or with no [`request_id`] layer.
    ///
    /// ```
    /// use quoll::server::RequestId;
    ///
    /// /// The id to log beside an error, when there is one.
    /// fn request() -> String {
    ///     RequestId::current().map_or_else(|| "none".to_owned(), |id| id.to_string())
    /// }
    /// # assert_eq!(request(), "none");
    /// ```
    pub fn current() -> Option<RequestId> {
        CURRENT.try_with(Clone::clone).ok()
    }

    /// The id, as it travels in `X-Request-Id`.
    pub fn as_str(&self) -> &str {
        self.0
            .to_str()
            .expect("a request id is printable ASCII, which `is_kept` and `made` check")
    }

    /// The caller's id when it is one to keep, or a new one.
    fn of(headers: &HeaderMap) -> Self {
        let mut given = headers.get_all(&X_REQUEST_ID).iter();

        match (given.next(), given.next()) {
            (Some(id), None) if is_kept(id.as_bytes()) => Self(id.clone()),
            _ => Self(made()),
        }
    }
}

impl fmt::Display for RequestId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

/// Whether a caller's id is kept: 1 to 128 bytes of printable ASCII, `!` to `~`, with no space. Any other, given
/// twice, too long, with a space, a control character or a byte that is not ASCII, is replaced: a log line shows an
/// id as it is, and an id a caller writes must not forge another line.
fn is_kept(id: &[u8]) -> bool {
    (1..=MAX_LEN).contains(&id.len()) && id.iter().all(|byte| byte.is_ascii_graphic())
}

/// A new id, written as a UUID v4, `8f14e45f-ceea-467a-9d1e-5b1c2b6b8e23`, which log tools read.
///
/// Its 122 random bits are a keyed hash of a counter: the keys are drawn once per process by `RandomState`, so ids
/// differ across processes and never repeat within one, and nothing can fail at a request, as a call to the system's
/// randomness may.
fn made() -> HeaderValue {
    static KEYS: LazyLock<(RandomState, RandomState)> =
        LazyLock::new(|| (RandomState::new(), RandomState::new()));
    static COUNTER: AtomicU64 = AtomicU64::new(0);

    let count = COUNTER.fetch_add(1, Ordering::Relaxed);
    let random = (u128::from(KEYS.0.hash_one(count)) << 64) | u128::from(KEYS.1.hash_one(count));
    // The version, 4, in the 13th digit, and the variant, `10` in binary, in the two high bits of the 17th.
    let bits = (random & !((0xf << 76) | (0b11 << 62))) | (0x4 << 76) | (0b10 << 62);
    let text = format!(
        "{:08x}-{:04x}-{:04x}-{:04x}-{:012x}",
        bits >> 96,
        (bits >> 80) & 0xffff,
        (bits >> 64) & 0xffff,
        (bits >> 48) & 0xffff,
        bits & 0xffff_ffff_ffff,
    );

    HeaderValue::try_from(text).expect("hex digits and `-` are a header's value")
}

/// Gives each request an id: the caller's `X-Request-Id` when it is one to keep, or a new one, a UUID v4. A caller's
/// id is kept when it is 1 to 128 bytes of printable ASCII with no space; one given twice, or any other, is replaced,
/// and the request served.
///
/// The id is set as the request's `X-Request-Id`, sent back as the answer's, and given to the span of [`trace`]
/// (whichever of the two layers runs first) and to the handler, through [`RequestId::current`].
///
/// ```
/// use quoll::server;
///
/// # fn api_router() -> axum::Router { axum::Router::new() }
/// let router = server::builder()
///     .with(server::request_id())
///     .with(server::trace())
///     .into_router(api_router());
/// ```
///
/// [`trace`]: super::trace()
pub fn request_id() -> RequestIdLayer {
    RequestIdLayer(())
}

/// The layer [`request_id`] gives.
#[derive(Debug, Clone)]
pub struct RequestIdLayer(());

impl<S> Layer<S> for RequestIdLayer {
    type Service = RequestIdService<S>;

    fn layer(&self, inner: S) -> Self::Service {
        RequestIdService { inner }
    }
}

/// The service of [`RequestIdLayer`].
#[derive(Debug, Clone)]
pub struct RequestIdService<S> {
    inner: S,
}

impl<S> Service<Request> for RequestIdService<S>
where
    S: Service<Request, Error = Infallible> + Send + 'static,
    S::Response: IntoResponse,
    S::Future: Send + 'static,
{
    type Response = Response;
    type Error = Infallible;
    type Future = ResponseFuture;

    fn poll_ready(&mut self, cx: &mut Context<'_>) -> Poll<Result<(), Infallible>> {
        self.inner.poll_ready(cx)
    }

    fn call(&mut self, mut request: Request) -> ResponseFuture {
        let id = RequestId::of(request.headers());

        request.headers_mut().insert(X_REQUEST_ID, id.0.clone());
        trace::record_request_id(request.extensions(), &id);
        request.extensions_mut().insert(id.clone());

        let answer = CURRENT.sync_scope(id.clone(), || self.inner.call(request));

        Box::pin(CURRENT.scope(id.clone(), async move {
            let mut response = answer.await?.into_response();

            response.headers_mut().insert(X_REQUEST_ID, id.0);
            Ok(response)
        }))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_made_id_is_a_uuid_v4() {
        let id = RequestId(made());
        let text = id.as_str();
        let groups: Vec<usize> = text.split('-').map(str::len).collect();

        assert_eq!(groups, [8, 4, 4, 4, 12], "{text}");
        assert!(text.chars().all(|c| c == '-' || c.is_ascii_hexdigit()));
        assert_eq!(&text[14..15], "4", "{text}");
        assert!("89ab".contains(&text[19..20]), "{text}");
    }

    #[test]
    fn made_ids_differ() {
        let ids: std::collections::HashSet<_> = (0..1000).map(|_| made()).collect();

        assert_eq!(ids.len(), 1000);
    }

    #[test]
    fn a_caller_id_is_kept_when_printable_and_short() {
        assert!(is_kept(b"abc-123"));
        assert!(is_kept(&[b'a'; MAX_LEN]));
        assert!(!is_kept(b""));
        assert!(!is_kept(&[b'a'; MAX_LEN + 1]));
        assert!(!is_kept(b"a b"));
        assert!(!is_kept(b"a\tb"));
        assert!(!is_kept("é".as_bytes()));
    }

    #[test]
    fn an_id_given_twice_is_replaced() {
        let mut headers = HeaderMap::new();
        headers.append(&X_REQUEST_ID, HeaderValue::from_static("one"));
        headers.append(&X_REQUEST_ID, HeaderValue::from_static("two"));

        let id = RequestId::of(&headers);

        assert_ne!(id.as_str(), "one");
        assert_ne!(id.as_str(), "two");
    }
}
