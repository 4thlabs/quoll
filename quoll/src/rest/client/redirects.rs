//! Redirects followed one hop at a time, through the middleware after them, so that a credential goes to the api's
//! own origin only.

use http::header::{
    AUTHORIZATION, CONTENT_ENCODING, CONTENT_LENGTH, CONTENT_TYPE, COOKIE, LOCATION,
    PROXY_AUTHORIZATION, TRANSFER_ENCODING,
};
use http::{Extensions, HeaderMap, HeaderName, Method, StatusCode};
use reqwest::{Request, Response, Url};
use reqwest_middleware::{Middleware, Next};

use super::middleware;

/// Follows up to `hops` redirects of each call, each hop sent through the middleware added after it, on a reqwest
/// client that follows none, `redirect(Policy::none())`: reqwest's own policy follows them below every middleware,
/// with the headers of the first request, which `key` and `on_request` may have added.
///
/// So each hop is given its credential, or not, again: [`bearer`](super::bearer), [`bearer_from`](super::bearer_from),
/// [`basic`](super::basic), [`key`](super::key) and [`on_request`](super::on_request) act on the api's own origin
/// only, and a hop to another origin gets none of them. A hop to another origin also leaves out the headers that
/// carry a credential, `Authorization`, `Cookie` and `Proxy-Authorization`, and every header marked sensitive, which
/// a middleware added before it would have put in.
///
/// - 301, 302 and 303 are followed with a `GET`, a `HEAD` staying one, and without the body, as browsers do;
/// - 307 and 308 with the method and the body, when the body can be sent twice, which a route's always can; else the
///   redirect is the answer;
/// - any other answer, a redirect with no `Location`, or one to a URL that is not `http` or `https`, is the answer;
/// - past `hops` redirects, or a redirect to a request already sent, the same method on the same URL, the call fails
///   with an error with no status.
///
/// ```no_run
/// # use quoll::rest;
/// # fn run() -> Result<(), reqwest::Error> {
/// let http = reqwest::Client::builder()
///     .redirect(reqwest::redirect::Policy::none())
///     .build()?;
/// let http = reqwest_middleware::ClientBuilder::new(http)
///     .with(rest::client::redirects(5))
///     .with(rest::client::bearer("mF_9.B5f-4.1JqM"))
///     .build();
/// # Ok(())
/// # }
/// ```
pub fn redirects(hops: usize) -> impl Middleware {
    middleware(move |request, extensions, next| Box::pin(follow(request, extensions, next, hops)))
}

/// Sends `request`, and each redirect it gets, up to `hops`.
async fn follow(
    mut request: Request,
    extensions: &mut Extensions,
    next: Next<'_>,
    hops: usize,
) -> reqwest_middleware::Result<Response> {
    let mut sent = vec![(request.method().clone(), request.url().clone())];

    loop {
        // A copy for a 307 or a 308, taken before it is sent; `None` for a body that streams.
        let again = request.try_clone();
        let without_body = without_body(&request);
        let from = request.url().clone();

        let response = next.clone().run(request, extensions).await?;
        let Some(to) = location(&response, &from) else {
            return Ok(response);
        };

        let mut hop = match response.status() {
            StatusCode::TEMPORARY_REDIRECT | StatusCode::PERMANENT_REDIRECT => {
                let Some(again) = again else {
                    return Ok(response);
                };

                again
            }
            _ => without_body,
        };
        *hop.url_mut() = to;

        let key = (hop.method().clone(), hop.url().clone());
        if sent.contains(&key) {
            return Err(Unfollowed::fail(
                "the call was redirected to a request it had sent already",
            ));
        }
        if sent.len() > hops {
            return Err(Unfollowed::fail(format!(
                "the call was redirected more than {hops} times"
            )));
        }

        if hop.url().origin() != from.origin() {
            strip_credentials(hop.headers_mut());
        }

        drop(response);
        sent.push(key);
        request = hop;
    }
}

/// Where a 301, 302, 303, 307 or 308 sends the call, resolved against the URL it answered: `None` for any other
/// answer, a redirect with no `Location`, or one that is not an `http` or `https` URL.
fn location(response: &Response, from: &Url) -> Option<Url> {
    let redirect = matches!(
        response.status(),
        StatusCode::MOVED_PERMANENTLY
            | StatusCode::FOUND
            | StatusCode::SEE_OTHER
            | StatusCode::TEMPORARY_REDIRECT
            | StatusCode::PERMANENT_REDIRECT
    );
    if !redirect {
        return None;
    }

    let location = response.headers().get(LOCATION)?.to_str().ok()?;
    let mut to = from.join(location).ok()?;

    to.set_fragment(None);
    matches!(to.scheme(), "http" | "https").then_some(to)
}

/// What a 301, 302 or 303 sends: `request` as a `GET`, a `HEAD` staying one, with its headers but those of its body,
/// and without the body.
fn without_body(request: &Request) -> Request {
    let method = if request.method() == Method::HEAD {
        Method::HEAD
    } else {
        Method::GET
    };
    let mut hop = Request::new(method, request.url().clone());

    let mut headers = request.headers().clone();
    for name in [
        CONTENT_TYPE,
        CONTENT_LENGTH,
        CONTENT_ENCODING,
        TRANSFER_ENCODING,
    ] {
        headers.remove(name);
    }

    *hop.headers_mut() = headers;
    *hop.timeout_mut() = request.timeout().copied();
    *hop.version_mut() = request.version();
    hop
}

/// Removes the headers that carry a credential, as reqwest does on a redirect to another host, and every header
/// marked sensitive.
fn strip_credentials(headers: &mut HeaderMap) {
    let names: Vec<HeaderName> = headers
        .iter()
        .filter(|(_, value)| value.is_sensitive())
        .map(|(name, _)| name.clone())
        .chain([AUTHORIZATION, COOKIE, PROXY_AUTHORIZATION])
        .collect();

    for name in names {
        headers.remove(name);
    }
}

/// A redirect not followed, past the limit or to a request already sent.
#[derive(Debug)]
struct Unfollowed(String);

impl Unfollowed {
    fn fail(message: impl Into<String>) -> reqwest_middleware::Error {
        reqwest_middleware::Error::middleware(Self(message.into()))
    }
}

impl std::fmt::Display for Unfollowed {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Unfollowed {}

#[cfg(test)]
mod tests {
    use super::*;

    use http::HeaderValue;

    #[test]
    fn a_redirect_to_another_origin_leaves_out_every_credential() {
        let mut headers = HeaderMap::new();
        let mut key = HeaderValue::from_static("k-1");
        key.set_sensitive(true);
        headers.insert("x-api-key", key);
        headers.insert(AUTHORIZATION, HeaderValue::from_static("Bearer t"));
        headers.insert(COOKIE, HeaderValue::from_static("session=s"));
        headers.insert(PROXY_AUTHORIZATION, HeaderValue::from_static("Basic a"));
        headers.insert("x-request-id", HeaderValue::from_static("id-1"));

        strip_credentials(&mut headers);

        assert_eq!(headers.len(), 1);
        assert_eq!(headers["x-request-id"], "id-1");
    }

    #[test]
    fn a_post_redirected_by_a_303_is_a_get_without_its_body() {
        let url = Url::parse("http://localhost/api/notes").expect("a url");
        let mut request = Request::new(Method::POST, url);
        request
            .headers_mut()
            .insert(CONTENT_TYPE, HeaderValue::from_static("application/json"));
        request
            .headers_mut()
            .insert("x-request-id", HeaderValue::from_static("id-1"));
        *request.body_mut() = Some("{}".into());

        let hop = without_body(&request);

        assert_eq!(hop.method(), Method::GET);
        assert!(hop.body().is_none());
        assert!(!hop.headers().contains_key(CONTENT_TYPE));
        assert_eq!(hop.headers()["x-request-id"], "id-1");

        *request.method_mut() = Method::HEAD;
        assert_eq!(without_body(&request).method(), Method::HEAD);
    }
}
