//! A tracing span around each call, on reqwest-tracing.

use http::Extensions;
use reqwest::{Request, Response};
use reqwest_middleware::Middleware;
use reqwest_tracing::{
    ReqwestOtelSpanBackend, TracingMiddleware, default_on_request_end, default_span_name,
    reqwest_otel_span,
};
use tracing::Span;

use crate::rest::Route;

/// Opens a tracing span around each call, `HTTP request`, with the fields of OpenTelemetry's conventions, as
/// reqwest-tracing writes them: `otel.name` is the operation id, `notes_get`, which an OpenTelemetry layer names the
/// span by, and `operation_id` and `group` say it too. A request that did not come from the client of a contract is
/// named by its method.
///
/// The span records the status of the answer, or the error of a call that got none. It holds no URL, whose query may
/// hold a credential. Added first, it is one span around the whole call, its attempts and its redirects.
///
/// ```no_run
/// # use quoll::rest;
/// let http = reqwest_middleware::ClientBuilder::new(reqwest::Client::new())
///     .with(rest::client::trace())
///     .build();
/// ```
pub fn trace() -> impl Middleware {
    TracingMiddleware::<Operation>::new()
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
        default_on_request_end(span, outcome);
    }
}
