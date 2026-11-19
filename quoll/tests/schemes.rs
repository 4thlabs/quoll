//! The schemes beside `Bearer`, from the contract to the wire: `Basic`, the schemes a contract declares, a scheme of
//! the application's own, alternatives, optional credentials and scopes, each served on a free port and called by the
//! real client and its middleware.

#![cfg(all(feature = "server", feature = "client"))]

use std::future::Future;
use std::pin::Pin;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};

use http::Extensions;
use http::request::Parts;
use quoll::rest::{
    self, Authenticate, Basic, BasicCredential, Bearer, Error, Result, Route, Scheme, Scoped, Text,
};
use reqwest::StatusCode;
use reqwest::header::{AUTHORIZATION, COOKIE, WWW_AUTHENTICATE};
use reqwest_middleware::{ClientBuilder, ClientWithMiddleware, Middleware, Next};
use serde_json::Value;

use other::Other as _;

rest::api! {
    /// Every scheme, to test them with.
    api Api = "/api";

    /// The key of an operator's scripts.
    scheme Key = header "X-Api-Key";
    /// The session of the web page.
    scheme Session = cookie "session";
    scheme Token = query "token";
    scheme Login = openid "https://accounts.example.com/.well-known/openid-configuration";

    group Admin {
        #[auth(Basic)]
        GET    "/basic"           => basic() -> String;
        #[auth(Key)]
        GET    "/key"             => key() -> String;
        #[auth(Session)]
        GET    "/session"         => session() -> String;
        #[auth(Session)]
        POST   "/session"         => post_session(text: Text) -> String;
        #[auth(Token)]
        GET    "/token?page={page}" => token(page: Option<u32>) -> String;
        #[auth(Login)]
        GET    "/login"           => login() -> String;
        #[auth(Hmac)]
        GET    "/signed"          => signed() -> String;
        #[auth(Bearer | Key)]
        GET    "/either"          => either() -> String;
        #[auth(Key | Bearer)]
        GET    "/key-first"       => key_first() -> String;
        #[auth(Bearer | Key, optional)]
        GET    "/maybe"           => maybe() -> String;
        #[auth(Bearer, scopes("repo", "admin"))]
        DELETE "/repo"            => remove();
        GET    "/open"            => open() -> String;
    }
}

/// Another contract, which declares a scheme of the same name.
mod other {
    use quoll::rest;

    rest::api! {
        pub api Api = "/other";

        pub scheme Key = header "X-Other-Key";

        #[auth(Key)]
        pub group Other {
            GET "/key" => key() -> String;
        }
    }
}

/// A request signed with a key the server knows: `X-Signature: <key id>:<signature>`.
struct Hmac;

/// What a signed request carries.
struct Signed {
    key_id: String,
    signature: String,
}

impl Scheme for Hmac {
    type Credential = Signed;

    const NAME: &'static str = "Hmac";

    fn read(request: &Parts) -> Result<Option<Signed>> {
        let Some(value) = request.headers.get("x-signature") else {
            return Ok(None);
        };
        let text = value
            .to_str()
            .map_err(|_| Error::unauthorized("the signature is not text"))?;
        let Some((key_id, signature)) = text.split_once(':') else {
            return Err(Error::unauthorized(
                "the signature is `<key id>:<signature>`",
            ));
        };

        Ok(Some(Signed {
            key_id: key_id.into(),
            signature: signature.into(),
        }))
    }
}

/// Who a request comes from, with the scopes its credential grants.
struct User {
    name: String,
    scopes: Vec<&'static str>,
}

impl User {
    fn new(name: &str, scopes: &[&'static str]) -> Self {
        Self {
            name: name.into(),
            scopes: scopes.to_vec(),
        }
    }
}

impl Scoped for User {
    fn has_scope(&self, scope: &str) -> bool {
        self.scopes.contains(&scope)
    }
}

/// The service, which counts the calls of its handlers and keeps the last page the token route read.
#[derive(Clone, Default)]
struct App {
    handled: Arc<AtomicUsize>,
    page: Arc<Mutex<Option<u32>>>,
}

impl App {
    fn handled(&self, user: &User) -> Result<String> {
        self.handled.fetch_add(1, Ordering::SeqCst);

        Ok(user.name.clone())
    }
}

/// The user a credential stands for, the same whatever its scheme: `ana`, or `admin` with every scope.
fn known(credential: &str) -> Result<User> {
    match credential {
        "ana" => Ok(User::new("ana", &["repo"])),
        "admin" => Ok(User::new("admin", &["repo", "admin"])),
        "down" => Err(Error::internal("the session store is down")),
        _ => Err(Error::unauthorized("unknown credential")),
    }
}

impl Authenticate<Basic> for App {
    type Caller = User;

    async fn authenticate(&self, login: BasicCredential) -> Result<User> {
        match (login.user.as_str(), login.password.as_str()) {
            ("ana", "secret") => known("ana"),
            _ => Err(Error::unauthorized("wrong user or password")),
        }
    }
}

impl Authenticate<Bearer> for App {
    type Caller = User;

    async fn authenticate(&self, token: String) -> Result<User> {
        known(token.strip_prefix("b-").unwrap_or_default())
    }
}

impl Authenticate<Key> for App {
    type Caller = User;

    async fn authenticate(&self, key: String) -> Result<User> {
        known(key.strip_prefix("k-").unwrap_or_default())
    }
}

impl Authenticate<Session> for App {
    type Caller = User;

    async fn authenticate(&self, session: String) -> Result<User> {
        known(session.strip_prefix("s-").unwrap_or_default())
    }
}

impl Authenticate<Token> for App {
    type Caller = User;

    async fn authenticate(&self, token: String) -> Result<User> {
        known(token.strip_prefix("t-").unwrap_or_default())
    }
}

impl Authenticate<Login> for App {
    type Caller = User;

    async fn authenticate(&self, token: String) -> Result<User> {
        known(token.strip_prefix("l-").unwrap_or_default())
    }
}

impl Authenticate<Hmac> for App {
    type Caller = User;

    async fn authenticate(&self, signed: Signed) -> Result<User> {
        match signed.signature.as_str() {
            "good" => known(&signed.key_id),
            _ => Err(Error::unauthorized("wrong signature")),
        }
    }
}

impl AdminServer for App {
    async fn basic(&self, user: &User) -> Result<String> {
        self.handled(user)
    }

    async fn key(&self, user: &User) -> Result<String> {
        self.handled(user)
    }

    async fn session(&self, user: &User) -> Result<String> {
        self.handled(user)
    }

    async fn post_session(&self, user: &User, text: Text) -> Result<String> {
        self.handled(user).map(|name| format!("{name}: {}", text.0))
    }

    async fn token(&self, user: &User, page: Option<u32>) -> Result<String> {
        *self.page.lock().expect("no test panics while holding it") = page;

        self.handled(user)
    }

    async fn login(&self, user: &User) -> Result<String> {
        self.handled(user)
    }

    async fn signed(&self, user: &User) -> Result<String> {
        self.handled(user)
    }

    async fn either(&self, user: &User) -> Result<String> {
        self.handled(user)
    }

    async fn key_first(&self, user: &User) -> Result<String> {
        self.handled(user)
    }

    async fn maybe(&self, user: Option<&User>) -> Result<String> {
        match user {
            Some(user) => self.handled(user),
            None => {
                self.handled.fetch_add(1, Ordering::SeqCst);

                Ok("anonymous".into())
            }
        }
    }

    async fn remove(&self, user: &User) -> Result<()> {
        self.handled(user).map(drop)
    }

    async fn open(&self) -> Result<String> {
        self.handled.fetch_add(1, Ordering::SeqCst);

        Ok("open".into())
    }
}

/// The service of the other contract.
#[derive(Clone)]
struct OtherApp;

impl Authenticate<other::Key> for OtherApp {
    type Caller = String;

    async fn authenticate(&self, key: String) -> Result<String> {
        Ok(key)
    }
}

impl other::OtherServer for OtherApp {
    async fn key(&self, key: &String) -> Result<String> {
        Ok(key.clone())
    }
}

/// A served api: its URL, the count of its handlers' calls and the service.
struct Served {
    url: String,
    app: App,
}

impl Served {
    fn handled(&self) -> usize {
        self.app.handled.load(Ordering::SeqCst)
    }

    /// The URL of a route, for a request sent by hand.
    fn at(&self, path: &str) -> String {
        format!("{}/api{path}", self.url)
    }
}

/// Serves both contracts on a free port.
async fn serve() -> Served {
    let app = App::default();
    let router = Api::router(ApiServices { admin: app.clone() })
        .merge(other::Api::router(other::ApiServices { other: OtherApp }));
    let url = rest::test::spawn(router).await;

    Served { url, app }
}

/// A client of the api through `middleware`.
fn client(url: &str, middleware: impl Middleware) -> Result<ApiClient> {
    let http = ClientBuilder::new(reqwest::Client::new())
        .with(middleware)
        .build();

    Api::client_with(http, url)
}

/// The challenges of an answer, in their order.
fn challenges(response: &reqwest::Response) -> Vec<String> {
    response
        .headers()
        .get_all(WWW_AUTHENTICATE)
        .iter()
        .map(|value| value.to_str().expect("a challenge is text").to_owned())
        .collect()
}

/// What an answer refused with `status` says: its detail, once its status and its content type are checked.
async fn refused(response: reqwest::Response, status: StatusCode, case: &str) -> String {
    assert_eq!(response.status(), status, "{case}");
    assert_eq!(
        response.headers()["content-type"],
        "application/problem+json",
        "{case}"
    );

    let problem: Value = response.json().await.expect("a problem");

    problem["detail"].as_str().unwrap_or_default().to_owned()
}

#[tokio::test]
async fn basic_reads_a_user_and_a_password() -> Result<()> {
    let served = serve().await;

    let api = client(&served.url, rest::client::basic("ana", "secret"))?;
    assert_eq!(api.admin().basic().await?, "ana");

    let http = reqwest::Client::new();
    for (case, header) in [
        ("none", None),
        ("a wrong password", Some("Basic YW5hOndyb25n")),
        ("not base64", Some("Basic ***")),
    ] {
        let mut request = http.get(served.at("/basic"));
        if let Some(header) = header {
            request = request.header(AUTHORIZATION, header);
        }
        let response = request.send().await.expect("the server answers");

        assert_eq!(
            challenges(&response),
            [r#"Basic realm="api", charset="UTF-8""#],
            "{case}"
        );
        refused(response, StatusCode::UNAUTHORIZED, case).await;
    }

    assert_eq!(served.handled(), 1);
    Ok(())
}

#[tokio::test]
async fn a_header_scheme_reads_its_header() -> Result<()> {
    let served = serve().await;

    let api = client(&served.url, rest::client::key::<Key>("k-ana"))?;
    assert_eq!(api.admin().key().await?, "ana");

    let response = reqwest::get(served.at("/key"))
        .await
        .expect("the server answers");
    assert_eq!(challenges(&response), ["Key"]);
    refused(response, StatusCode::UNAUTHORIZED, "no key").await;

    let wrong = client(&served.url, rest::client::key::<Key>("k-nobody"))?;
    let error = wrong.admin().key().await.expect_err("an unknown key");
    assert_eq!(error.status(), Some(StatusCode::UNAUTHORIZED));

    assert_eq!(served.handled(), 1);
    Ok(())
}

/// A middleware that adds the cookie `theme=dark` to each request, before the one that sends a session.
fn theme() -> impl Middleware {
    middleware(|mut request, extensions, next| {
        request
            .headers_mut()
            .insert(COOKIE, http::HeaderValue::from_static("theme=dark"));

        Box::pin(next.run(request, extensions))
    })
}

#[tokio::test]
async fn a_cookie_scheme_reads_its_cookie_beside_the_others() -> Result<()> {
    let served = serve().await;
    let seen = Seen::default();
    let http = ClientBuilder::new(reqwest::Client::new())
        .with(theme())
        .with(rest::client::key::<Session>("s-ana"))
        .with(recorder(Arc::clone(&seen)))
        .build();
    let api = Api::client_with(http, &served.url)?;

    assert_eq!(api.admin().session().await?, "ana");
    // A client that is no browser sends no `Sec-Fetch-Site` and no `Origin`.
    assert_eq!(
        api.admin().post_session(Text("milk".into())).await?,
        "ana: milk"
    );

    let cookies: Vec<Option<String>> = seen
        .lock()
        .expect("no test panics while holding it")
        .iter()
        .map(|seen| seen.cookie.clone())
        .collect();
    assert_eq!(
        cookies,
        [
            Some("theme=dark; session=s-ana".into()),
            Some("theme=dark; session=s-ana".into())
        ]
    );

    let response = reqwest::get(served.at("/session"))
        .await
        .expect("the server answers");
    assert_eq!(challenges(&response), ["Session"]);
    refused(response, StatusCode::UNAUTHORIZED, "no cookie").await;

    assert_eq!(served.handled(), 2);
    Ok(())
}

#[tokio::test]
async fn a_cookie_from_another_origin_is_refused_on_a_route_that_changes_something() {
    let served = serve().await;
    let http = reqwest::Client::new();
    let host = served.url.trim_start_matches("http://").to_owned();
    let post = |headers: &[(&str, String)]| {
        let mut request = http
            .post(served.at("/session"))
            .header(COOKIE, "session=s-ana")
            .header("content-type", "text/plain")
            .body("milk");
        for (name, value) in headers {
            request = request.header(*name, value);
        }

        request.send()
    };

    for (case, headers) in [
        (
            "cross-site",
            vec![("sec-fetch-site", "cross-site".to_owned())],
        ),
        (
            "same-site",
            vec![("sec-fetch-site", "same-site".to_owned())],
        ),
        (
            "another origin",
            vec![("origin", "https://evil.example".to_owned())],
        ),
        ("an opaque origin", vec![("origin", "null".to_owned())]),
    ] {
        let response = post(&headers).await.expect("the server answers");

        assert!(challenges(&response).is_empty(), "{case}");
        let detail = refused(response, StatusCode::FORBIDDEN, case).await;
        assert!(detail.contains("another origin"), "{case}: {detail}");
    }

    for (case, headers) in [
        (
            "same-origin",
            vec![("sec-fetch-site", "same-origin".to_owned())],
        ),
        (
            "typed by the user",
            vec![("sec-fetch-site", "none".to_owned())],
        ),
        ("its own origin", vec![("origin", format!("http://{host}"))]),
        ("no browser", vec![]),
    ] {
        let response = post(&headers).await.expect("the server answers");

        assert_eq!(response.status(), StatusCode::OK, "{case}");
    }

    // A read from another site changes nothing: the browser keeps its answer from the page, without CORS.
    let read = http
        .get(served.at("/session"))
        .header(COOKIE, "session=s-ana")
        .header("sec-fetch-site", "cross-site")
        .send()
        .await
        .expect("the server answers");
    assert_eq!(read.status(), StatusCode::OK);

    assert_eq!(served.handled(), 5);
}

#[tokio::test]
async fn a_query_scheme_reads_its_key_beside_the_routes_own() -> Result<()> {
    let served = serve().await;
    let seen = Seen::default();
    let http = ClientBuilder::new(reqwest::Client::new())
        .with(rest::client::key::<Token>("t-ana"))
        .with(recorder(Arc::clone(&seen)))
        .build();
    let api = Api::client_with(http, &served.url)?;

    assert_eq!(api.admin().token(Some(2)).await?, "ana");
    assert_eq!(
        *served
            .app
            .page
            .lock()
            .expect("no test panics while holding it"),
        Some(2)
    );
    assert_eq!(api.admin().token(None).await?, "ana");

    let queries: Vec<Option<String>> = seen
        .lock()
        .expect("no test panics while holding it")
        .iter()
        .map(|seen| seen.query.clone())
        .collect();
    assert_eq!(
        queries,
        [
            Some("page=2&token=t-ana".into()),
            Some("token=t-ana".into())
        ]
    );

    let response = reqwest::get(served.at("/token?page=2"))
        .await
        .expect("the server answers");
    assert_eq!(challenges(&response), ["Token"]);
    refused(response, StatusCode::UNAUTHORIZED, "no token").await;
    Ok(())
}

#[tokio::test]
async fn an_openid_scheme_reads_a_bearer_token_that_bearer_does_not_send() -> Result<()> {
    let served = serve().await;

    let api = client(&served.url, rest::client::key::<Login>("l-ana"))?;
    assert_eq!(api.admin().login().await?, "ana");

    // `bearer` sends its token to the routes that take `Bearer`, which this one does not.
    let bearer = client(&served.url, rest::client::bearer("l-ana"))?;
    let error = bearer.admin().login().await.expect_err("no token sent");
    assert_eq!(error.status(), Some(StatusCode::UNAUTHORIZED));

    let response = reqwest::get(served.at("/login"))
        .await
        .expect("the server answers");
    assert_eq!(challenges(&response), ["Bearer"]);
    Ok(())
}

#[tokio::test]
async fn a_scheme_of_the_applications_own_is_read_by_its_type() {
    let served = serve().await;
    let http = reqwest::Client::new();

    let signed = http
        .get(served.at("/signed"))
        .header("x-signature", "ana:good")
        .send()
        .await
        .expect("the server answers");
    assert_eq!(signed.text().await.expect("a body"), "\"ana\"");

    for (case, signature) in [
        ("none", None),
        ("wrong", Some("ana:bad")),
        ("malformed", Some("ana")),
    ] {
        let mut request = http.get(served.at("/signed"));
        if let Some(signature) = signature {
            request = request.header("x-signature", signature);
        }
        let response = request.send().await.expect("the server answers");

        assert_eq!(challenges(&response), ["Hmac"], "{case}");
        refused(response, StatusCode::UNAUTHORIZED, case).await;
    }
}

#[tokio::test]
async fn an_alternative_takes_the_first_credential_the_request_carries() {
    let served = serve().await;
    let http = reqwest::Client::new();
    let call = |path: &'static str, bearer: Option<&'static str>, key: Option<&'static str>| {
        let mut request = http.get(served.at(path));
        if let Some(bearer) = bearer {
            request = request.header(AUTHORIZATION, bearer);
        }
        if let Some(key) = key {
            request = request.header("x-api-key", key);
        }

        request.send()
    };

    let cases = [
        ("/either", Some("Bearer b-ana"), None, Some("ana")),
        ("/either", None, Some("k-admin"), Some("admin")),
        // `Bearer` comes first: its credential decides, and the key is not tried.
        (
            "/either",
            Some("Bearer b-ana"),
            Some("k-nobody"),
            Some("ana"),
        ),
        ("/either", Some("Bearer b-nobody"), Some("k-admin"), None),
        ("/either", Some("Bearer not a token"), Some("k-admin"), None),
        // Another scheme in `Authorization` is no credential of `Bearer`.
        (
            "/either",
            Some("Basic YW5hOnNlY3JldA=="),
            Some("k-admin"),
            Some("admin"),
        ),
        ("/either", None, None, None),
        // `Key` comes first here.
        ("/key-first", Some("Bearer b-ana"), Some("k-nobody"), None),
        (
            "/key-first",
            Some("Bearer b-nobody"),
            Some("k-admin"),
            Some("admin"),
        ),
    ];

    for (path, bearer, key, expected) in cases {
        let case = format!("{path} {bearer:?} {key:?}");
        let response = call(path, bearer, key).await.expect("the server answers");

        let Some(name) = expected else {
            let order = if path == "/either" {
                ["Bearer", "Key"]
            } else {
                ["Key", "Bearer"]
            };
            assert_eq!(challenges(&response), order, "{case}");
            refused(response, StatusCode::UNAUTHORIZED, &case).await;
            continue;
        };

        assert_eq!(response.status(), StatusCode::OK, "{case}");
        assert_eq!(
            response.text().await.expect("a body"),
            format!("\"{name}\""),
            "{case}"
        );
    }
}

#[tokio::test]
async fn an_optional_route_takes_no_credential_but_refuses_a_wrong_one() -> Result<()> {
    let served = serve().await;

    assert_eq!(
        Api::client(&served.url)?.admin().maybe().await?,
        "anonymous"
    );
    assert_eq!(
        client(&served.url, rest::client::bearer("b-ana"))?
            .admin()
            .maybe()
            .await?,
        "ana"
    );
    assert_eq!(
        client(&served.url, rest::client::key::<Key>("k-admin"))?
            .admin()
            .maybe()
            .await?,
        "admin"
    );

    for (token, status) in [
        ("b-nobody", StatusCode::UNAUTHORIZED),
        ("b-down", StatusCode::INTERNAL_SERVER_ERROR),
    ] {
        let error = client(&served.url, rest::client::bearer(token))?
            .admin()
            .maybe()
            .await
            .expect_err("a wrong token");

        assert_eq!(error.status(), Some(status), "{token}");
    }

    assert_eq!(served.handled(), 3);
    Ok(())
}

#[tokio::test]
async fn a_caller_needs_every_scope_of_its_route() -> Result<()> {
    let served = serve().await;

    client(&served.url, rest::client::bearer("b-admin"))?
        .admin()
        .remove()
        .await?;
    assert_eq!(served.handled(), 1);

    let response = reqwest::Client::new()
        .delete(served.at("/repo"))
        .header(AUTHORIZATION, "Bearer b-ana")
        .send()
        .await
        .expect("the server answers");
    assert!(challenges(&response).is_empty());
    let detail = refused(response, StatusCode::FORBIDDEN, "no scope admin").await;
    assert_eq!(
        detail,
        "the caller lacks the scope `admin`, which this route takes"
    );

    let error = Api::client(&served.url)?
        .admin()
        .remove()
        .await
        .expect_err("no credential");
    assert_eq!(error.status(), Some(StatusCode::UNAUTHORIZED));

    assert_eq!(served.handled(), 1);
    Ok(())
}

#[tokio::test]
async fn a_middleware_tells_two_schemes_of_one_name_apart() -> Result<()> {
    let served = serve().await;
    let http = ClientBuilder::new(reqwest::Client::new())
        .with(rest::client::key::<Key>("k-ana"))
        .build();
    let api = Api::client_with(http.clone(), &served.url)?;
    let other = other::Api::client_with(http, &served.url)?;

    assert_eq!(api.admin().key().await?, "ana");

    let error = other
        .other()
        .key()
        .await
        .expect_err("the other contract's key");
    assert_eq!(error.status(), Some(StatusCode::UNAUTHORIZED));

    let both = ClientBuilder::new(reqwest::Client::new())
        .with(rest::client::key::<Key>("k-ana"))
        .with(rest::client::key::<other::Key>("o-1"))
        .build();
    assert_eq!(
        other::Api::client_with(both, &served.url)?
            .other()
            .key()
            .await?,
        "o-1"
    );
    Ok(())
}

#[tokio::test]
async fn a_route_names_its_schemes_and_tells_their_types() -> Result<()> {
    let served = serve().await;
    let seen = Seen::default();
    let http = ClientBuilder::new(reqwest::Client::new())
        .with(recorder(Arc::clone(&seen)))
        .build();
    let api = Api::client_with(http, &served.url)?;

    let _ = api.admin().either().await;
    let _ = api.admin().login().await;

    let seen = seen.lock().expect("no test panics while holding it");
    let either: &[&str] = &["Bearer", "Key"];
    let login: &[&str] = &["Login"];
    assert_eq!(seen[0].schemes, Some(either));
    assert_eq!(seen[0].takes, [true, true, false, false]);
    assert_eq!(seen[1].schemes, Some(login));
    assert_eq!(seen[1].takes, [false, false, true, false]);
    Ok(())
}

#[tokio::test]
async fn a_credential_the_server_would_not_read_fails_the_call_without_showing_it() -> Result<()> {
    let served = serve().await;
    let unsendable: [(&str, ClientWithMiddleware); 7] = [
        ("basic user", with(rest::client::basic("secret:user", "x"))),
        (
            "basic password",
            with(rest::client::basic("ana", "secret\n")),
        ),
        ("header", with(rest::client::key::<Key>("secret\nkey"))),
        ("header blank", with(rest::client::key::<Key>(" secret"))),
        ("cookie", with(rest::client::key::<Session>("secret;key"))),
        ("query", with(rest::client::key::<Token>(""))),
        ("openid", with(rest::client::key::<Login>("secret token"))),
    ];

    for (case, http) in unsendable {
        let api = Api::client_with(http, &served.url)?;
        let call = match case {
            "basic user" | "basic password" => api.admin().basic().await,
            "header" | "header blank" => api.admin().key().await,
            "cookie" => api.admin().session().await,
            "query" => api.admin().token(None).await,
            _ => api.admin().login().await,
        };

        let error = call.expect_err("not sendable");
        let shown = format!("{error} {error:?}");
        assert_eq!(error.status(), None, "{case}: {shown}");
        assert!(!shown.contains("secret"), "{case}: {shown}");

        // A route that takes no such scheme is called as it is.
        assert_eq!(api.admin().open().await?, "open", "{case}");
    }

    assert_eq!(served.handled(), 7);
    Ok(())
}

/// A client with one middleware.
fn with(middleware: impl Middleware) -> ClientWithMiddleware {
    ClientBuilder::new(reqwest::Client::new())
        .with(middleware)
        .build()
}

/// What a middleware saw of a request.
#[derive(Debug, Default)]
struct Request {
    schemes: Option<&'static [&'static str]>,
    takes: [bool; 4],
    cookie: Option<String>,
    query: Option<String>,
}

type Seen = Arc<Mutex<Vec<Request>>>;

/// What a middleware's call returns.
type Handled<'a> =
    Pin<Box<dyn Future<Output = reqwest_middleware::Result<reqwest::Response>> + Send + 'a>>;

/// Gives a closure the signature reqwest-middleware takes, for every lifetime of its request.
fn middleware<F>(handle: F) -> F
where
    F: for<'a> Fn(reqwest::Request, &'a mut Extensions, Next<'a>) -> Handled<'a>
        + Send
        + Sync
        + 'static,
{
    handle
}

/// A middleware that keeps what it sees of each request, put last so that it sees what the others sent.
fn recorder(seen: Seen) -> impl Middleware {
    middleware(move |request, extensions, next| {
        let route = extensions.get::<Route>();
        let takes = route.map_or([false; 4], |route| {
            [
                route.takes::<Bearer>(),
                route.takes::<Key>(),
                route.takes::<Login>(),
                route.takes::<other::Key>(),
            ]
        });

        seen.lock()
            .expect("no test panics while holding it")
            .push(Request {
                schemes: route.map(Route::schemes),
                takes,
                cookie: request
                    .headers()
                    .get(COOKIE)
                    .map(|cookie| cookie.to_str().expect("a cookie is text").to_owned()),
                query: request.url().query().map(str::to_owned),
            });

        Box::pin(next.run(request, extensions))
    })
}
