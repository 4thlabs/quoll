# Another API

A contract can describe an API that quoll did not write, and that the application only calls: the API of GitHub, of
Stripe, or of a weather service. It is written like any other contract. A few attributes say what such an API asks
for: its headers, the shape of its errors, the keys of its queries. This guide writes a contract of GitHub's REST API,
then goes through what other APIs bring.

## A contract of GitHub

```rust,no_run
use std::fmt;

use quoll::{Bytes, client};
use serde::{Deserialize, Serialize};

/// A repository, with the fields the application reads.
#[derive(Debug, Serialize, Deserialize)]
pub struct Repository {
    pub full_name: String,
    pub stargazers_count: u64,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct Issue {
    pub number: u64,
    pub title: String,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum State {
    Open,
    Closed,
    All,
}

/// What GitHub answers on an error.
#[derive(Debug, Deserialize)]
pub struct GitHubError {
    pub message: String,
    pub documentation_url: Option<String>,
}

impl fmt::Display for GitHubError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

/// The version of GitHub's REST API the contract follows.
pub const VERSION: &str = "2022-11-28";

quoll::api! {
    /// GitHub's REST API, at `https://api.github.com`.
    #[openapi(none)]
    #[header("Accept" = "application/vnd.github+json")]
    #[header("X-GitHub-Api-Version" = VERSION)]
    #[error(GitHubError)]
    pub api GitHub = "";

    pub group Repos {
        GET "/repos/{owner}/{repo}" => get(owner: String, repo: String) -> Repository;

        #[header("Accept" = "application/vnd.github.raw+json")]
        GET "/repos/{owner}/{repo}/readme" => readme(owner: String, repo: String) -> Bytes;
    }

    pub group Issues {
        GET "/repos/{owner}/{repo}/issues?labels={labels}&state={state}&per_page={per_page}&page={page}"
            => list(
                owner: String,
                repo: String,
                #[join(",")] labels: Vec<String>,
                state: Option<State>,
                per_page: Option<u8>,
                page: Option<u32>,
            ) -> Vec<Issue>;
    }
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let github = client::builder()
        .with(client::user_agent("quoll-guide"))
        .build::<GitHub>("https://api.github.com")?;

    let repo = github.repos().get("rust-lang".into(), "rust".into()).await?;

    println!("{} has {} stars", repo.full_name, repo.stargazers_count);

    let readme = github.repos().readme("rust-lang".into(), "rust".into()).await?;
    let readme = String::from_utf8(readme.into_vec())?;

    println!("{readme}");

    let labels = vec!["C-bug".into(), "T-compiler".into()];
    let issues = github
        .issues()
        .list("rust-lang".into(), "rust".into(), labels, Some(State::Open), Some(5), None)
        .await?;

    for issue in issues {
        println!("#{} {}", issue.number, issue.title);
    }

    Ok(())
}
```

- `#[openapi(none)]`: the contract has no OpenAPI document, and its types need only serde, not `JsonSchema`. A crate
  with only the feature `client` needs `Deserialize` on what it reads and `Serialize` on what it sends. The guides also
  turn on `server`, so their types derive both. A type holds the fields that the application reads: serde skips the
  others.
- The prefix is `""`, and the client's URL is the base of the API: `https://api.github.com`, or
  `https://github.example.com/api/v3` for a GitHub Enterprise server. The client keeps that path before the path of
  each route.
- `#[header(...)]` on the api sends GitHub's media type and version on every call. The README's route asks for another
  media type, which replaces the `Accept` of the api.
- `#[error(GitHubError)]` reads GitHub's errors: `another api answered 404: Not Found`.
- `#[join(",")]` writes the labels as one value, `labels=C-bug,T-compiler`, which is how GitHub reads them.
- GitHub refuses a request with no `User-Agent`, with a 403: `client::user_agent` sends one.

The sections below take each of these in turn, and what other APIs also ask for.

## Names

The name of a group is the name of its client's trait, `Repos`, in the contract's module. A group with the same name
as a type there does not compile, which happens quickly when you copy an API's names:

```rust,compile_fail,E0428
use serde::Deserialize;

#[derive(Debug, Deserialize)]
pub struct Places {
    pub results: Vec<String>,
}

quoll::api! {
    #[openapi(none)]
    pub api Geocoding = "/v1";

    pub group Places {
        GET "/search?name={name}" => search(name: String) -> Places;
    }
}
```

rustc says `the name Places is defined multiple times`. One of the two takes another name: here the group becomes
`Search`, as in [Several hosts](#several-hosts).

## Headers

A header that the API asks of every call, such as a version or a media type, is fixed in the contract: see
[Fixed headers](contract.md#fixed-headers). Some cases come back from one API to the next:

- The API's own media type, such as `application/vnd.github+json`, replaces quoll's `Accept` on every route under it.
  A route whose result is `Text`, `Events` or `JsonLines` takes `#[header("Accept" = none)]`, to ask for its own
  again.
- `Text` reads only `text/plain`. Some APIs answer a text in a media type of their own, such as GitHub's
  `application/vnd.github.raw+json; charset=utf-8`. Such a text is read as `Bytes`, of any content type.
  `String::from_utf8(bytes.into_vec())` turns the bytes into text, or into an error when they are not UTF-8.
- A parameter of the body's `Content-Type`, such as Slack's `charset=utf-8`, is fixed like any header:
  `#[header("Content-Type" = "application/json; charset=utf-8")]`. A request with no body sends no `Content-Type`.
- A header whose value changes from call to call, such as Stripe Connect's `Stripe-Account` or Anthropic's
  `anthropic-beta`, is an argument of the route: `header("anthropic-beta") beta: Option<String>`. A `None` leaves it
  out.
- A header that comes from a deployment, a tenant or a signature goes through
  [`client::on_request`](client.md#a-header-computed-from-the-request).

## Errors

Each API answers its errors in a shape of its own, and `#[error(E)]` says it once for the api. The `Display` of `E`
gives the error's message, wherever the shape keeps it:

| API        | An error's body                                   | The message     |
| ---------- | ------------------------------------------------- | --------------- |
| GitHub     | `{"message", "documentation_url"}`                | `message`       |
| Stripe     | `{"error": {"type", "code", "message", "param"}}` | `error.message` |
| Anthropic  | `{"type": "error", "error": {"type", "message"}}` | `error.message` |
| Open-Meteo | `{"error": true, "reason"}`                       | `reason`        |

```rust
use std::fmt;

use serde::Deserialize;

/// What Stripe answers on an error.
#[derive(Debug, Deserialize)]
pub struct StripeError {
    pub error: StripeErrorBody,
}

#[derive(Debug, Deserialize)]
pub struct StripeErrorBody {
    #[serde(rename = "type")]
    pub kind: String,
    pub code: Option<String>,
    pub message: String,
}

impl fmt::Display for StripeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.error.message)
    }
}
```

[Another API's errors](errors.md#another-apis-errors) says what `error.detail()` gives. It also says how a route that
answers another shape, such as an OAuth endpoint, says its own.

### An error in a 200

Slack answers its errors with a 200 and `"ok": false` in the body. The contract cannot say that a 200 is an error. So
the result is an envelope, read by `ok`, which the caller turns into a `Result`.

```rust
use std::fmt;

use serde::de::{self, DeserializeOwned, Deserializer};
use serde::{Deserialize, Serialize};

/// Every answer of Slack: the method's own fields when `ok` is `true`, the error otherwise.
#[derive(Debug, Serialize)]
#[serde(untagged)]
pub enum Slack<T> {
    Ok(T),
    Err(SlackError),
}

impl<'de, T: DeserializeOwned> Deserialize<'de> for Slack<T> {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        let answer = serde_json::Value::deserialize(deserializer)?;

        let read = if answer["ok"] == true {
            serde_json::from_value(answer).map(Slack::Ok)
        } else {
            serde_json::from_value(answer).map(Slack::Err)
        };

        read.map_err(de::Error::custom)
    }
}

/// What Slack answers on an error, with a 200 or another status.
#[derive(Debug, Serialize, Deserialize)]
pub struct SlackError {
    pub error: String,
}

impl fmt::Display for SlackError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.error)
    }
}

impl std::error::Error for SlackError {}

impl<T> Slack<T> {
    /// The method's fields, or the error Slack answered.
    pub fn into_result(self) -> Result<T, SlackError> {
        match self {
            Slack::Ok(data) => Ok(data),
            Slack::Err(error) => Err(error),
        }
    }
}

#[derive(Debug, Serialize, Deserialize)]
pub struct Identity {
    pub user_id: String,
    pub team: String,
}

quoll::api! {
    /// Slack's Web API, at `https://slack.com`.
    #[openapi(none)]
    #[error(SlackError)]
    pub api SlackApi = "/api";

    #[auth(Bearer)]
    pub group Auth {
        POST "/auth.test" => test() -> Slack<Identity>;
    }
}

async fn whoami(slack: &SlackApiClient) -> anyhow::Result<Identity> {
    let identity = slack.auth().test().await?.into_result()?;

    Ok(identity)
}

fn main() {
    let refused: Slack<Identity> = serde_json::from_str(r#"{"ok": false, "error": "not_authed"}"#).unwrap();
    let unread = serde_json::from_str::<Slack<Identity>>(r#"{"ok": true, "user": "U1"}"#);

    assert_eq!(refused.into_result().unwrap_err().to_string(), "not_authed");
    assert!(unread.unwrap_err().to_string().contains("missing field `user_id`"));
}
```

- `SlackError` is the shape of the errors that Slack answers with another status, including a 429. It is also the
  shape of the error in the envelope: one type for both.
- An answer with `"ok": true` that does not hold the method's fields is an answer that is not the route's result. The
  call fails, with serde's error as its source.
- The api is named `SlackApi`. An api's name is also a type of the module, and the envelope already has the name
  `Slack`.

## Results

A route returns what the API answers on a success. Two cases are special:

- A route that the API answers with a 204 returns a value that may be empty: nothing, an `Option<T>`, a `Text`,
  `Bytes` or a stream (see [Results](contract.md#results)). The client reads the 204 as `()`, `None`, an empty text
  or bytes, or a stream that ends at once. GitHub's `GET /user/starred/{owner}/{repo}` answers 204 when the user
  starred the repository, and 404 when not. A route `-> T`, whose `T` is a JSON value, fails when it is answered with
  a 204: that answer is not its result. Its [`.response()`](client.md#the-whole-answer) keeps the status and the
  headers.
- A route that an API answers in two shapes, JSON or a stream, depending on a field of its body, is two routes of one
  method and path: [Two answers on one path](#two-answers-on-one-path) says how.

```rust
use std::fmt;

use http::StatusCode;
use serde::Deserialize;

#[derive(Debug, Deserialize)]
pub struct GitHubError {
    pub message: String,
}

impl fmt::Display for GitHubError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

quoll::api! {
    #[openapi(none)]
    #[header("Accept" = "application/vnd.github+json")]
    #[error(GitHubError)]
    pub api GitHub = "";

    #[auth(Bearer)]
    pub group Stars {
        /// 204 when the signed-in user starred the repository, 404 otherwise.
        GET "/user/starred/{owner}/{repo}" => check(owner: String, repo: String);
    }
}

async fn starred(github: &GitHubClient, owner: &str, repo: &str) -> quoll::Result<bool, GitHubError> {
    match github.stars().check(owner.into(), repo.into()).await {
        Ok(()) => Ok(true),
        Err(error) if error.status() == Some(StatusCode::NOT_FOUND) => Ok(false),
        Err(error) => Err(error),
    }
}
# let _ = starred;
```

## Queries and forms

- A key with brackets, such as Stripe's `expand[]` or JSON:API's `filter[name]`, is written in the route's string:
  see [Query parameters](contract.md#query-parameters).
- A list that the API reads as one value, such as `labels=bug,docs`, is an argument with `#[join(sep)]`: see
  [Joined lists](contract.md#joined-lists). A field of a query struct is not joined, so a route whose list is joined
  takes the list as an argument.

### A nested form

Stripe reads a map as keys with brackets, such as `metadata[order_id]=42`, in a form or a query. A field that is a
map fails at the call, because a form has no nesting. So the map holds the keys as Stripe writes them, and serde's
`flatten` puts them beside the other fields.

```rust,no_run
use std::collections::BTreeMap;

use quoll::{Form, client};
use serde::{Deserialize, Serialize};

#[derive(Debug, Default, Serialize, Deserialize)]
pub struct NewCustomer {
    pub email: Option<String>,
    /// The keys `nested("metadata", ..)` builds, `metadata[order_id]`.
    #[serde(flatten)]
    pub metadata: BTreeMap<String, String>,
    #[serde(rename = "expand[]", default)]
    pub expand: Vec<String>,
}

/// The keys of a map nested in a form of Stripe's, `name[key]`.
pub fn nested<'a>(name: &str, map: impl IntoIterator<Item = (&'a str, &'a str)>) -> BTreeMap<String, String> {
    map.into_iter()
        .map(|(key, value)| (format!("{name}[{key}]"), value.to_owned()))
        .collect()
}

#[derive(Debug, Serialize, Deserialize)]
pub struct Customer {
    pub id: String,
}

quoll::api! {
    /// Stripe's API, at `https://api.stripe.com`.
    #[openapi(none)]
    pub api Stripe = "/v1";

    #[auth(Bearer)]
    pub group Customers {
        POST "/customers"
            => create(header("Idempotency-Key") key: Option<String>, customer: Form<NewCustomer>) -> Customer;
    }
}

#[tokio::main]
async fn main() -> quoll::Result<()> {
    let stripe = client::builder()
        .with(client::bearer("sk_test_..."))
        .build::<Stripe>("https://api.stripe.com")?;

    let customer = NewCustomer {
        email: Some("ada@example.com".into()),
        metadata: nested("metadata", [("order_id", "42")]),
        expand: vec!["tax".into()],
    };
    let created = stripe
        .customers()
        .create(Some("order-42".into()), Form(customer))
        .await?;

    println!("{}", created.id);
    Ok(())
}
```

The form sent is `email=ada%40example.com&metadata%5Border_id%5D=42&expand%5B%5D=tax`, with the brackets escaped
like a browser's form escapes them. A list of objects, such as `items[0][price]`, is written the same way, with its
keys numbered.

## The whole answer

The headers of an answer, such as a rate limit, a request id or a `Link`, come with the whole answer: `.response()`.
A call that takes longer than the others, such as a long message from an LLM, takes a deadline of its own:
`.timeout(..)`. [The whole answer](client.md#the-whole-answer) shows both.

## Retries

`client::retry(times)` tries again a failure with no answer, a 502, a 503 or a 504, on the methods that may be sent
twice. An API says which of its errors pass, and `.when(..)` follows it. For example: a 429 after its `Retry-After`,
Anthropic's 529, or a `POST` that carries Stripe's `Idempotency-Key`.

```rust,no_run
use http::Method;
use quoll::client::{self, Attempt, Retry};

fn http() -> quoll::Result<reqwest_middleware::ClientWithMiddleware> {
    let retry = client::retry(3).when(|attempt: &Attempt| {
        let request = attempt.request();
        let keyed = request.method() == Method::POST && request.headers().contains_key("Idempotency-Key");

        match attempt.status() {
            // Too many requests: the API did nothing, and says when to come back.
            Some(status) if status.as_u16() == 429 => attempt.retry_after(),
            // Anthropic is overloaded, and did nothing either.
            Some(status) if status.as_u16() == 529 => Retry::Backoff,
            // A POST that Stripe does once per key.
            Some(status) if keyed && status.is_server_error() => Retry::Backoff,
            _ => attempt.transient(),
        }
    });

    client::builder().with(retry).build_http()
}
```

[Deciding a retry](client.md#deciding-a-retry) says what an `Attempt` holds.

## Two answers on one path

Some APIs answer one method and path in two shapes, chosen by a field of the body. Anthropic's `POST /v1/messages`
answers a message in JSON, or server-sent events when its body says `"stream": true`. Each answer is a route. On each
route, `#[body("stream" = ...)]` fixes the field that tells them apart, and the client adds that field to the body.
[Fixed fields of the body](contract.md#fixed-fields-of-the-body) says how:

```rust,no_run
use std::fmt;
use std::time::Duration;

use futures::StreamExt;
use quoll::{Call, Events, client};
use serde::{Deserialize, Serialize};

/// The version of the Messages API the contract follows.
pub const VERSION: &str = "2023-06-01";

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct MessageRequest {
    pub model: String,
    pub max_tokens: u32,
    pub messages: Vec<Input>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Input {
    pub role: String,
    pub content: String,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct Message {
    pub content: Vec<Content>,
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum Content {
    Text { text: String },
    #[serde(other)]
    Other,
}

/// An event of a streamed message, named by its `type`.
#[derive(Debug, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum StreamEvent {
    ContentBlockDelta { delta: Delta },
    MessageStop,
    /// A failure after the 200, `overloaded_error` among them.
    Error { error: AnthropicErrorBody },
    #[serde(other)]
    Other,
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum Delta {
    TextDelta { text: String },
    #[serde(other)]
    Other,
}

/// What Anthropic answers on an error.
#[derive(Debug, Deserialize)]
pub struct AnthropicError {
    pub error: AnthropicErrorBody,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct AnthropicErrorBody {
    #[serde(rename = "type")]
    pub kind: String,
    pub message: String,
}

impl fmt::Display for AnthropicError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.error.message)
    }
}

quoll::api! {
    /// Anthropic's API, at `https://api.anthropic.com`.
    #[openapi(none)]
    #[header("anthropic-version" = VERSION)]
    #[error(AnthropicError)]
    pub api Anthropic = "/v1";

    /// A key of the Claude Console.
    pub scheme ApiKey = header "x-api-key";

    #[auth(ApiKey)]
    pub group Messages {
        #[body("stream" = false)]
        POST "/messages" => create(request: MessageRequest) -> Message;

        #[body("stream" = true)]
        POST "/messages" => stream(request: MessageRequest) -> Events<StreamEvent>;
    }
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let key = std::env::var("ANTHROPIC_API_KEY")?;
    let http = client::builder().with(client::key::<ApiKey>(key)).build_http()?;
    let anthropic = Anthropic::client_with(http, "https://api.anthropic.com")?;

    let request = MessageRequest {
        model: "claude-opus-5-5".into(),
        max_tokens: 1024,
        messages: vec![Input {
            role: "user".into(),
            content: "Name three Australian marsupials.".into(),
        }],
    };

    let message = anthropic
        .messages()
        .create(request.clone())
        .timeout(Duration::from_secs(600))
        .await?;

    for content in message.content {
        if let Content::Text { text } = content {
            println!("{text}");
        }
    }

    let mut events = anthropic.messages().stream(request).await?;

    while let Some(event) = events.next().await {
        match event? {
            StreamEvent::ContentBlockDelta {
                delta: Delta::TextDelta { text },
            } => print!("{text}"),
            StreamEvent::Error { error } => anyhow::bail!("the message failed: {}", error.message),
            _ => {}
        }
    }

    Ok(())
}
```

- `create` sends `"stream": false` and `stream` sends `"stream": true`: the caller never sets it, and
  `MessageRequest` has no field for it. If the body had a field of that name, the contract's value would replace it.
- Two routes of one method and path compile only in an api `#[openapi(none)]`, which a contract of another API
  usually is: the document does not describe them yet.
- Anthropic's `event: error`, a failure once the stream has begun, is an item like any other. The enum names it
  `Error`, and the loop turns it into an error. `Other` is for the events that the client skips: `ping`, and the
  events that the API adds later, so that a new event breaks no client. It is never for an error.

## Several hosts

Some APIs are served on several hosts, such as Open-Meteo's geocoding and its forecast. Such an API is one api, with
one group per host. It has one client per host, and the clients share one `ClientWithMiddleware` (see
[Building a client](client.md#building-a-client)):

```rust,no_run
use std::fmt;

use quoll::client;
use serde::{Deserialize, Serialize};

#[derive(Debug, Serialize, Deserialize)]
pub struct Places {
    #[serde(default)]
    pub results: Vec<Place>,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct Place {
    pub name: String,
    pub latitude: f64,
    pub longitude: f64,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct Forecast {
    pub hourly: Hourly,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct Hourly {
    pub time: Vec<String>,
    pub temperature_2m: Vec<Option<f64>>,
}

/// What Open-Meteo answers on an error.
#[derive(Debug, Deserialize)]
pub struct OpenMeteoError {
    pub reason: String,
}

impl fmt::Display for OpenMeteoError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.reason)
    }
}

quoll::api! {
    /// Open-Meteo's API, its geocoding at `https://geocoding-api.open-meteo.com` and its forecast at
    /// `https://api.open-meteo.com`.
    #[openapi(none)]
    #[error(OpenMeteoError)]
    pub api OpenMeteo = "/v1";

    pub group Search {
        GET "/search?name={name}&count={count}" => search(name: String, count: Option<u8>) -> Places;
    }

    pub group Forecasts {
        GET "/forecast?latitude={latitude}&longitude={longitude}&hourly={hourly}&forecast_days={days}"
            => get(latitude: f64, longitude: f64, #[join(",")] hourly: Vec<String>, days: Option<u8>) -> Forecast;
    }
}

#[tokio::main]
async fn main() -> quoll::Result<()> {
    let http = client::builder()
        .with(client::user_agent("quoll-guide"))
        .with(client::retry(3))
        .build_http()?;
    let geocoding = OpenMeteo::client_with(http.clone(), "https://geocoding-api.open-meteo.com")?;
    let weather = OpenMeteo::client_with(http, "https://api.open-meteo.com")?;

    let places = geocoding.search().search("Hobart".into(), Some(1)).await?;
    let Some(hobart) = places.results.first() else {
        return Ok(());
    };

    let hourly = vec!["temperature_2m".into()];
    let forecast = weather
        .forecasts()
        .get(hobart.latitude, hobart.longitude, hourly, Some(1))
        .await?;

    for (time, temperature) in forecast.hourly.time.iter().zip(&forecast.hourly.temperature_2m) {
        println!("{time} {temperature:?} °C");
    }

    Ok(())
}
```

Each client serves the group of its host: `geocoding.search()` on the geocoding host, and `weather.forecasts()` on
the forecast host.

## Pages

quoll has no helper for pages yet. The caller loops, one call per page, until the API says there is no page after.
GitHub says it in the `Link` header of the answer, which `.response()` gives:

```rust
use std::fmt;

use http::header::LINK;
use quoll::Call;
use serde::{Deserialize, Serialize};

#[derive(Debug, Serialize, Deserialize)]
pub struct Issue {
    pub number: u64,
    pub title: String,
}

#[derive(Debug, Deserialize)]
pub struct GitHubError {
    pub message: String,
}

impl fmt::Display for GitHubError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

quoll::api! {
    #[openapi(none)]
    #[header("Accept" = "application/vnd.github+json")]
    #[error(GitHubError)]
    pub api GitHub = "";

    pub group Issues {
        GET "/repos/{owner}/{repo}/issues?per_page={per_page}&page={page}"
            => list(owner: String, repo: String, per_page: Option<u8>, page: Option<u32>) -> Vec<Issue>;
    }
}

async fn all_issues(github: &GitHubClient, owner: &str, repo: &str) -> quoll::Result<Vec<Issue>, GitHubError> {
    let mut issues = Vec::new();

    for page in 1.. {
        let response = github
            .issues()
            .list(owner.into(), repo.into(), Some(100), Some(page))
            .response()
            .await?;
        let link = response.headers().get(LINK).and_then(|link| link.to_str().ok());
        let next = link.is_some_and(|link| link.contains(r#"rel="next""#));

        issues.extend(response.into_value()?);

        if !next {
            break;
        }
    }

    Ok(issues)
}
# let _ = all_issues;
```

Some APIs give their cursor in the body: Stripe's `has_more` and the id to start after, or Slack's
`response_metadata.next_cursor`. They are looped the same way, and each call takes the cursor of the page before.
Helpers are to come: pages by the `Link` header, [#23](https://github.com/4thlabs/quoll/issues/23), and by a cursor in the
body, [#80](https://github.com/4thlabs/quoll/issues/80).

## Fakes

The client of a group implements its trait, `Repos`, and so does a fake. A fake's errors have the API's shape:
`quoll::Error::answered(StatusCode::NOT_FOUND, error)`. [Testing](testing.md#fakes) and
[Another API's errors](errors.md#another-apis-errors) show one. A quoll server answers its errors as problems, whatever
the contract's `#[error]` says. So a fake server of another API, one that answers in its shape, is written with axum
alone.

## What a contract cannot say yet

- A segment that mixes a parameter and text, such as Google's `/v1/models/{model}:generateContent`:
  [#85](https://github.com/4thlabs/quoll/issues/85).
- A joined list in a field of a query struct: [#93](https://github.com/4thlabs/quoll/issues/93).
- A helper for pages: by the `Link` header, [#23](https://github.com/4thlabs/quoll/issues/23), or by a cursor in the
  body, [#80](https://github.com/4thlabs/quoll/issues/80).
