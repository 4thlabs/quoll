//! The builder of a client: the helpers of quoll in the order they work in, any other middleware where it is given,
//! and the reqwest client under them.

use std::any::Any;
use std::fmt;
use std::sync::Arc;

use reqwest::redirect::Policy;
use reqwest_middleware::{ClientWithMiddleware, Middleware};
use sync_wrapper::SyncWrapper;

use super::{Helper, Rank, redirects};
use crate::order::arrange;
use crate::rest::{Error, Result};

/// The redirects a builder given a credential and no `redirects` follows: reqwest's own limit.
const REDIRECTS: usize = 10;

/// What configures the reqwest client of a [`Builder`], called once, by value: never shared, so that it need not be
/// `Sync`.
type Configure =
    SyncWrapper<Box<dyn FnOnce(reqwest::ClientBuilder) -> reqwest::ClientBuilder + Send>>;

/// A builder of a client with no middleware yet, on a reqwest client with reqwest's defaults.
///
/// ```no_run
/// use quoll::{client, rest};
/// # rest::api! {
/// #     pub api Api = "/api";
/// #     #[auth(Bearer)]
/// #     pub group Notes { GET "/notes/{id}" => get(id: i64) -> String; }
/// # }
///
/// # async fn run() -> rest::Result<()> {
/// let api = client::builder()
///     .with(client::bearer("mF_9.B5f-4.1JqM"))
///     .with(client::retry(3))
///     .with(client::request_id())
///     .build::<Api>("http://localhost:8080")?;
///
/// let note = api.notes().get(1).await?;
/// # Ok(())
/// # }
/// ```
pub fn builder() -> Builder {
    Builder::default()
}

/// Builds the client of a contract, or a `ClientWithMiddleware`, from middleware and the settings of reqwest's
/// client: what `reqwest_middleware::ClientBuilder` does, with the helpers of quoll put in the order they work in.
///
/// The middleware run in the order they are given, the first seeing the request first and the answer last, except
/// the helpers of quoll, which take their places among themselves in this order, whatever the order they are given
/// in:
///
/// 1. [`trace`](super::trace), one span around the whole call;
/// 2. [`user_agent`](super::user_agent);
/// 3. [`request_id`](super::request_id), before `retry`, so that every attempt of a call has one id;
/// 4. [`retry`](super::retry);
/// 5. [`timeout`](super::timeout), a deadline per attempt and per hop;
/// 6. [`redirects`](super::redirects), before every credential, so that each hop is given it or not;
/// 7. the credentials, [`bearer`](super::bearer), [`bearer_from`](super::bearer_from), [`basic`](super::basic) and
///    [`key`](super::key), and [`on_request`](super::on_request), in the order they are given.
///
/// A middleware of the application's keeps its place: the helpers are sorted into the places they were given at, and
/// any other stays where it is. A middleware that adds a credential comes after `redirects`, as the helpers do. A
/// helper is told by its type: one wrapped in a middleware of the application's is a middleware of the application's.
///
/// Given a credential, `bearer`, `bearer_from`, `basic`, `key` or `on_request`, and no `redirects`, the builder adds
/// `redirects(10)` itself, reqwest's own limit, so that redirects are still followed, but each hop through the
/// credentials: reqwest's own policy follows them below every middleware, with the headers of the first request, and
/// keeps the key of a header scheme, or what `on_request` adds, on a hop to another origin. A `redirects(n)` given is
/// kept as it is.
///
/// ```no_run
/// use quoll::client;
/// use reqwest::Request;
/// # quoll::rest::api! { pub api Api = "/api"; pub group Notes { GET "/notes" => list() -> String; } }
///
/// # fn run() -> Result<(), Box<dyn std::error::Error>> {
/// let sign = client::on_request(|request: &mut Request, _: &quoll::rest::Route| {
///     request.headers_mut().insert("X-Signature", http::HeaderValue::from_static("s-1"));
/// });
/// let proxy = reqwest::Proxy::all("http://proxy.example.com:3128")?;
///
/// // `redirects` runs before `sign`, and `request_id` before `retry`.
/// let api = client::builder()
///     .http(move |http| http.proxy(proxy))
///     .with(sign)
///     .with(client::retry(3))
///     .with(client::request_id())
///     .with(client::redirects(5))
///     .build::<Api>("http://localhost:8080")?;
/// # Ok(())
/// # }
/// ```
#[derive(Default)]
pub struct Builder {
    http: Vec<Configure>,
    middleware: Vec<(Option<Rank>, Arc<dyn Middleware>)>,
}

impl Builder {
    /// Adds `middleware`: a helper of quoll's, at the place its kind works at, or any other, at the place it is given.
    pub fn with<M: Middleware>(mut self, middleware: M) -> Self {
        let rank = (&middleware as &dyn Any)
            .downcast_ref::<Helper>()
            .map(|helper| helper.rank);

        self.middleware.push((rank, Arc::new(middleware)));
        self
    }

    /// Configures the reqwest client under the middleware, with `configure`, given its `reqwest::ClientBuilder`: a
    /// proxy, TLS, a pool. Each closure given is called in turn, when the client is built.
    ///
    /// With [`redirects`](super::redirects), given or added for a credential, the reqwest client follows no
    /// redirect, whatever `configure` sets: `redirects` follows them itself. Without it, the client follows them as
    /// reqwest does, ten at most.
    ///
    /// ```no_run
    /// use std::time::Duration;
    ///
    /// use quoll::client;
    ///
    /// # fn run() -> quoll::rest::Result<()> {
    /// let http = client::builder()
    ///     .http(|http| http.pool_idle_timeout(Duration::from_secs(30)))
    ///     .build_http()?;
    /// # Ok(())
    /// # }
    /// ```
    pub fn http<F>(mut self, configure: F) -> Self
    where
        F: FnOnce(reqwest::ClientBuilder) -> reqwest::ClientBuilder + Send + 'static,
    {
        self.http.push(SyncWrapper::new(Box::new(configure)));
        self
    }

    /// Builds a client of the api `A` served at `url`, as `http://localhost:8080`, sending its requests through the
    /// middleware: what `Api::client_with` gives from the `ClientWithMiddleware` of [`build_http`](Self::build_http).
    ///
    /// # Errors
    ///
    /// When reqwest cannot build its client, or `url` is not an `http` or `https` URL with no query.
    pub fn build<A: Contract>(self, url: &str) -> Result<A::Client> {
        A::client_with(self.build_http()?, url)
    }

    /// Builds the `ClientWithMiddleware`, which `Api::client_with` takes: one for the clients of several contracts,
    /// which then share its connections.
    ///
    /// # Errors
    ///
    /// When reqwest cannot build its client, as when its TLS cannot be set up.
    pub fn build_http(mut self) -> Result<ClientWithMiddleware> {
        let given = |wanted: Rank| {
            self.middleware
                .iter()
                .any(|(rank, _)| *rank == Some(wanted))
        };
        let follows = given(Rank::Redirects);
        let credentials = given(Rank::Credential);

        if credentials && !follows {
            self.middleware
                .push((Some(Rank::Redirects), Arc::new(redirects(REDIRECTS))));
        }

        let mut http = reqwest::Client::builder();

        for configure in self.http {
            http = (configure.into_inner())(http);
        }

        if credentials || follows {
            http = http.redirect(Policy::none());
        }

        let http = http.build().map_err(|error| {
            Error::misuse("the http client cannot be built", Some(Box::new(error)))
        })?;
        let built = arrange(self.middleware).into_iter().fold(
            reqwest_middleware::ClientBuilder::new(http),
            |built, middleware| built.with_arc(middleware),
        );

        Ok(built.build())
    }
}

/// Shows how many middleware and settings it holds, which have no `Debug` of their own.
impl fmt::Debug for Builder {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Builder")
            .field("http", &self.http.len())
            .field("middleware", &self.middleware.len())
            .finish()
    }
}

/// The api of a contract, of which [`Builder::build`] builds a client: `rest::api!` implements it on the api's type,
/// `Api`, with the feature `client`.
#[diagnostic::on_unimplemented(
    message = "`{Self}` is not the api of a contract, of which `client::Builder::build` builds a client",
    label = "not the api of a contract",
    note = "the api is the type `rest::api!` names after `api`, as `Api` in `pub api Api = \"/api\";`, and it has a client when `quoll` has the feature `client`"
)]
pub trait Contract {
    /// The client of the api, `ApiClient`.
    type Client;

    /// A client of the api served at `url`, sending its requests through `http`: what `Api::client_with` does.
    ///
    /// # Errors
    ///
    /// When `url` is not an `http` or `https` URL with no query.
    fn client_with(http: ClientWithMiddleware, url: &str) -> Result<Self::Client>;
}
