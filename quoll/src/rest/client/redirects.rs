//! Redirects followed one hop at a time, through the middleware after them, so that a credential goes to the api's
//! own origin only.

use http::header::{
    AUTHORIZATION, CONTENT_ENCODING, CONTENT_LENGTH, CONTENT_TYPE, COOKIE, LOCATION,
    PROXY_AUTHORIZATION, TRANSFER_ENCODING,
};
use http::{Extensions, HeaderMap, HeaderName, Method, StatusCode};
use reqwest::{Request, Response, Url};
use reqwest_middleware::{Middleware, Next};

use super::request::HeadDeadline;
use super::{Failure, middleware};

/// Follows up to `hops` redirects of each call, each hop sent through the middleware added after it, on a reqwest
/// client that follows none, `redirect(Policy::none())`: reqwest's own policy follows them below every middleware,
/// with the headers of the first request, which `key` and `on_request` may have added.
///
/// It comes before every middleware that adds a credential, so that they run on each hop and send nothing to another
/// origin: [`bearer`](super::bearer), [`bearer_from`](super::bearer_from), [`basic`](super::basic),
/// [`key`](super::key) and [`on_request`](super::on_request) act on the api's own origin only. A hop is a copy of the
/// request `redirects` got, with what the middleware before it added: to another origin, it leaves out the headers
/// that carry a credential, `Authorization`, `Cookie` and `Proxy-Authorization`, and every header marked sensitive,
/// but keeps any other, so that a header of a credential added before it is to be marked sensitive.
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
        // A copy of the request as it is sent, for its redirect; of all but its body, as a 303 sends it, when the body
        // streams and cannot be copied.
        let (copy, whole) = match request.try_clone() {
            Some(copy) => (copy, true),
            None => (without_body(&request), false),
        };

        let response = next.clone().run(request, extensions).await?;
        let Some(to) = location(&response, copy.url()) else {
            return Ok(response);
        };

        let from = copy.url().origin();
        let keeps_body = matches!(
            response.status(),
            StatusCode::TEMPORARY_REDIRECT | StatusCode::PERMANENT_REDIRECT
        );

        if keeps_body && !whole {
            return Ok(response);
        }

        let mut hop = if keeps_body || !whole {
            copy
        } else {
            without_body(&copy)
        };

        *hop.url_mut() = to;

        let key = (hop.method().clone(), hop.url().clone());

        if sent.contains(&key) {
            return Err(Failure::fail(
                "the call was redirected to a request it had sent already",
            ));
        }

        if sent.len() > hops {
            return Err(Failure::fail(format!(
                "the call was redirected more than {hops} times"
            )));
        }

        if hop.url().origin() != from {
            strip_credentials(hop.headers_mut());
        }

        // The head of each hop of a stream's answer has its own deadline, as each hop has its own request.
        if let Some(deadline) = extensions.get::<HeadDeadline>() {
            deadline.restart();
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

    // As reqwest reads it: UTF-8, which the URL escapes, not only the ASCII of `to_str`.
    let location = std::str::from_utf8(response.headers().get(LOCATION)?.as_bytes()).ok()?;
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
