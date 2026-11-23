//! A tracing span around each call, on reqwest-tracing.

use std::error::Error as StdError;

use http::Extensions;
use reqwest::{Request, Response, Url};
use reqwest_middleware::Middleware;
use reqwest_tracing::{
    ERROR_CAUSE_CHAIN, ERROR_MESSAGE, OTEL_STATUS_CODE, ReqwestOtelSpanBackend, TracingMiddleware,
    default_on_request_success, default_span_name, reqwest_otel_span,
};
use tracing::Span;

use super::{Helper, Rank};
use crate::__private::client::redacted;
use crate::Route;

/// Opens a tracing span around each call, named by its operation.
///
/// The span is `HTTP request`, with the fields of OpenTelemetry's conventions, as reqwest-tracing writes them:
/// `otel.name` is the operation id, `notes_get`, which an OpenTelemetry layer names the span by, and `operation_id` and
/// `group` say it too. A request that did not come from the client of a contract is named by its method.
///
/// The span records the status of the answer, or the error of a call that got none. It holds no URL with its query,
/// which may hold a credential: the error's message, which reqwest writes with the URL, shows its origin and its path
/// alone. Added first, it is one span around the whole call, its attempts and its redirects.
///
/// ```no_run
/// # use quoll::client;
/// let http = client::builder()
///     .with(client::trace())
///     .build_http()?;
/// # Ok::<(), quoll::Error>(())
/// ```
pub fn trace() -> impl Middleware {
    Helper::new(Rank::Trace, TracingMiddleware::<Operation>::new())
}

/// The span of a call, named by its operation.
struct Operation;

impl ReqwestOtelSpanBackend for Operation {
    fn on_request_start(request: &Request, extensions: &mut Extensions) -> Span {
        let Some(route) = extensions.get::<Route>() else {
            let name = default_span_name(request, extensions);

            return reqwest_otel_span!(name = name, request);
        };

        reqwest_otel_span!(
            name = route.operation_id(),
            request,
            operation_id = route.operation_id(),
            group = route.group(),
        )
    }

    fn on_request_end(
        span: &Span,
        outcome: &reqwest_middleware::Result<Response>,
        _: &mut Extensions,
    ) {
        match outcome {
            Ok(response) => default_on_request_success(span, response),
            Err(error) => {
                let (message, chain) = without_urls(error);

                span.record(OTEL_STATUS_CODE, "ERROR");
                span.record(ERROR_MESSAGE, message.as_str());
                span.record(ERROR_CAUSE_CHAIN, chain.as_str());
            }
        }
    }
}

/// The message of `error`, and the messages of it and of each error it comes from joined with `: `, where each URL a
/// reqwest error names stands as its origin and its path, without its query, which may hold a credential.
fn without_urls(error: &reqwest_middleware::Error) -> (String, String) {
    let mut urls = Vec::new();

    urls_in(error, &mut urls);

    let redact = |message: String| {
        urls.iter().fold(message, |message, url| {
            message.replace(url.as_str(), &redacted(url))
        })
    };
    let mut chain = Vec::new();
    let mut current: Option<&(dyn StdError + 'static)> = Some(error);

    while let Some(error) = current {
        chain.push(redact(error.to_string()));
        current = error.source();
    }

    let message = chain.first().cloned().unwrap_or_default();

    (message, chain.join(": "))
}

/// Gathers in `urls` the URLs of the reqwest errors `error` holds, itself, through the errors it comes from, and
/// through those that show a reqwest error's message as their own, reqwest-middleware's, which leave it out of the
/// errors they come from.
fn urls_in(error: &(dyn StdError + 'static), urls: &mut Vec<Url>) {
    if let Some(error) = error.downcast_ref::<reqwest::Error>() {
        urls.extend(error.url().cloned());
    }

    match error.downcast_ref::<reqwest_middleware::Error>() {
        Some(reqwest_middleware::Error::Reqwest(error)) => urls.extend(error.url().cloned()),
        Some(reqwest_middleware::Error::Middleware(error)) => urls_in(&**error, urls),
        None => {}
    }

    if let Some(source) = error.source() {
        urls_in(source, urls);
    }
}
