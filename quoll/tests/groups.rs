//! A contract split over modules, `rest::group!` blocks that an api lists, against the same contract written in one
//! block: the same routes on the wire, the same `rest::Route` on each request, the same OpenAPI document; and a group
//! listed by two apis, each serving it under its own prefix.

#![cfg(all(feature = "server", feature = "client"))]

use std::sync::{Arc, Mutex};

use quoll::rest::{Authenticate, Created, Error, Form, Result, Route};
use reqwest::StatusCode;
use reqwest_middleware::{ClientBuilder, ClientWithMiddleware};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

/// A note.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
pub struct Note {
    /// Its id.
    pub id: i64,
    /// Its title.
    pub title: String,
}

/// A note to write.
#[derive(Debug, Serialize, Deserialize, JsonSchema)]
pub struct NewNote {
    /// Its title.
    pub title: String,
}

/// A login.
#[derive(Debug, Serialize, Deserialize, JsonSchema)]
pub struct Login {
    /// Who logs in.
    pub user: String,
}

/// The contract in modules: a group in each, and the api, which declares the scheme, in its own.
mod split {
    pub mod auth {
        use quoll::rest::{self, Form};

        use crate::Login;

        rest::group! {
            /// Logins.
            pub group Auth {
                /// Trades a login for a token.
                POST "/token" => token(login: Form<Login>) -> String;
            }
        }
    }

    pub mod notes {
        use quoll::rest::{self, Created};

        use crate::{NewNote, Note};

        rest::group! {
            /// The notes of an operator.
            #[auth(crate::split::Key)]
            pub group Notes {
                GET    "/notes/{id}"     => get(id: i64) -> Note;
                POST   "/notes"          => create(note: NewNote) -> Created<Note>;
                #[auth(none)]
                GET    "/notes?q={q}"    => search(q: Option<String>) -> Vec<Note>;
                DELETE "/notes/{id}"     => remove(id: i64);
            }
        }
    }

    use quoll::rest;

    rest::api! {
        /// Notes, in modules.
        pub api Api = "/api" { auth::Auth, notes::Notes };

        /// The key of an operator.
        pub scheme Key = header "X-Api-Key";

        pub group Health {
            GET "/" => root() -> String;
        }
    }

    /// The notes group again, under another prefix.
    pub mod other {
        use quoll::rest;

        rest::api! {
            pub api Other = "/other" { super::notes::Notes };
        }
    }
}

/// The same contract, in one block.
mod whole {
    use quoll::rest::{self, Created, Form};

    use crate::{Login, NewNote, Note};

    rest::api! {
        /// Notes, in modules.
        pub api Api = "/api";

        /// The key of an operator.
        pub scheme Key = header "X-Api-Key";

        /// Logins.
        pub group Auth {
            /// Trades a login for a token.
            POST "/token" => token(login: Form<Login>) -> String;
        }

        /// The notes of an operator.
        #[auth(Key)]
        pub group Notes {
            GET    "/notes/{id}"     => get(id: i64) -> Note;
            POST   "/notes"          => create(note: NewNote) -> Created<Note>;
            #[auth(none)]
            GET    "/notes?q={q}"    => search(q: Option<String>) -> Vec<Note>;
            DELETE "/notes/{id}"     => remove(id: i64);
        }

        pub group Health {
            GET "/" => root() -> String;
        }
    }
}

/// One service for both contracts: the traits of each are their own, the handlers the same.
#[derive(Clone)]
struct App;

impl App {
    fn note(id: i64) -> Result<Note> {
        match id {
            1 => Ok(Note {
                id,
                title: "Milk".into(),
            }),
            _ => Err(Error::not_found("no such note")),
        }
    }

    fn operator(key: &str) -> Result<String> {
        match key {
            "secret" => Ok("ana".into()),
            _ => Err(Error::unauthorized("unknown key")),
        }
    }
}

/// Implements a contract's traits for `App`.
macro_rules! serve {
    ($auth:ident, $notes:ident, $health:ident, $key:path) => {
        impl $auth for App {
            async fn token(&self, login: Form<Login>) -> Result<String> {
                Ok(format!("token of {}", login.user))
            }
        }

        impl $notes for App {
            async fn get(&self, _operator: &String, id: i64) -> Result<Note> {
                App::note(id)
            }

            async fn create(&self, _operator: &String, note: NewNote) -> Result<Created<Note>> {
                Ok(Created::at(
                    "/api/notes/2",
                    Note {
                        id: 2,
                        title: note.title,
                    },
                ))
            }

            async fn search(&self, q: Option<String>) -> Result<Vec<Note>> {
                Ok(q.map(|_| App::note(1)).transpose()?.into_iter().collect())
            }

            async fn remove(&self, _operator: &String, id: i64) -> Result<()> {
                App::note(id).map(|_| ())
            }
        }

        impl $health for App {
            async fn root(&self) -> Result<String> {
                Ok("up".into())
            }
        }

        impl Authenticate<$key> for App {
            type Caller = String;

            async fn authenticate(&self, key: String) -> Result<String> {
                App::operator(&key)
            }
        }
    };
}

use split::auth::AuthServer as SplitAuth;
use split::notes::NotesServer as SplitNotes;
use split::{HealthServer as SplitHealth, Key as SplitKey};
use whole::{
    AuthServer as WholeAuth, HealthServer as WholeHealth, Key as WholeKey,
    NotesServer as WholeNotes,
};

serve!(SplitAuth, SplitNotes, SplitHealth, SplitKey);
serve!(WholeAuth, WholeNotes, WholeHealth, WholeKey);

fn split_router() -> axum::Router {
    split::Api::router(split::ApiServices {
        auth: App,
        notes: App,
        health: App,
    })
}

fn whole_router() -> axum::Router {
    whole::Api::router(whole::ApiServices {
        auth: App,
        notes: App,
        health: App,
    })
}

/// What a client's middleware saw of each request's route: its operation, its group, its method, its path and its
/// schemes' names, which two contracts share; the types of the schemes are each contract's own.
type Seen = Arc<Mutex<Vec<(String, String, String, String, Vec<String>)>>>;

/// An HTTP client recording the route of each request, and sending the operator's key.
fn http(seen: &Seen) -> ClientWithMiddleware {
    let seen = Arc::clone(seen);
    let record = quoll::client::on_request(move |request: &mut reqwest::Request, route: &Route| {
        let schemes = route.schemes().iter().map(|name| (*name).to_owned());

        seen.lock().expect("no test panics with it").push((
            route.operation_id().to_owned(),
            route.group().to_owned(),
            route.method().to_string(),
            route.path().to_owned(),
            schemes.collect(),
        ));
        request.headers_mut().insert(
            "x-api-key",
            reqwest::header::HeaderValue::from_static("secret"),
        );
    });

    ClientBuilder::new(reqwest::Client::new())
        .with(record)
        .build()
}

/// Each answer of a client of the split contract against `url`, as text: the same calls give the same answers.
async fn split_calls(url: &str, seen: &Seen) -> Vec<String> {
    use split::Health as _;
    use split::auth::Auth as _;
    use split::notes::Notes as _;

    let api = split::Api::client_with(http(seen), url).expect("the URL is an api's");

    vec![
        format!(
            "{:?}",
            api.auth().token(Form(Login { user: "ana".into() })).await
        ),
        format!("{:?}", api.notes().get(1).await),
        format!(
            "{:?}",
            api.notes().get(7).await.map_err(|error| error.status())
        ),
        format!(
            "{:?}",
            api.notes()
                .create(NewNote {
                    title: "Tea".into()
                })
                .await
        ),
        format!("{:?}", api.notes().search(Some("milk".into())).await),
        format!("{:?}", api.notes().remove(1).await),
        format!("{:?}", api.health().root().await),
    ]
}

/// The same calls, through a client of the contract in one block.
async fn whole_calls(url: &str, seen: &Seen) -> Vec<String> {
    use whole::{Auth as _, Health as _, Notes as _};

    let api = whole::Api::client_with(http(seen), url).expect("the URL is an api's");

    vec![
        format!(
            "{:?}",
            api.auth().token(Form(Login { user: "ana".into() })).await
        ),
        format!("{:?}", api.notes().get(1).await),
        format!(
            "{:?}",
            api.notes().get(7).await.map_err(|error| error.status())
        ),
        format!(
            "{:?}",
            api.notes()
                .create(NewNote {
                    title: "Tea".into()
                })
                .await
        ),
        format!("{:?}", api.notes().search(Some("milk".into())).await),
        format!("{:?}", api.notes().remove(1).await),
        format!("{:?}", api.health().root().await),
    ]
}

#[tokio::test]
async fn a_split_contract_answers_as_the_contract_in_one_block() {
    let split_url = quoll::test::spawn(split_router()).await;
    let whole_url = quoll::test::spawn(whole_router()).await;
    let seen = Seen::default();

    // Each client against each server: the wire is the same.
    let split_on_split = split_calls(&split_url, &seen).await;
    let split_on_whole = split_calls(&whole_url, &seen).await;
    let whole_on_split = whole_calls(&split_url, &seen).await;
    let whole_on_whole = whole_calls(&whole_url, &seen).await;

    assert_eq!(split_on_split, whole_on_whole);
    assert_eq!(split_on_whole, whole_on_whole);
    assert_eq!(whole_on_split, whole_on_whole);
    assert_eq!(
        split_on_split[1], r#"Ok(Note { id: 1, title: "Milk" })"#,
        "the calls reach the handlers"
    );
    assert_eq!(split_on_split[2], "Err(Some(404))");
}

#[tokio::test]
async fn a_split_contract_gives_each_request_the_route_of_the_contract_in_one_block() {
    let url = quoll::test::spawn(split_router()).await;
    let (split_seen, whole_seen) = (Seen::default(), Seen::default());

    split_calls(&url, &split_seen).await;
    whole_calls(&url, &whole_seen).await;

    let split_seen = split_seen.lock().expect("no test panics with it").clone();
    let whole_seen = whole_seen.lock().expect("no test panics with it").clone();

    assert_eq!(split_seen, whole_seen);
    assert_eq!(
        split_seen[1],
        (
            "notes_get".to_owned(),
            "Notes".to_owned(),
            "GET".to_owned(),
            "/api/notes/{id}".to_owned(),
            vec!["Key".to_owned()],
        )
    );
    assert_eq!(split_seen[6].3, "/api", "the route `/` is the prefix");
}

#[tokio::test]
async fn a_split_contract_takes_the_schemes_its_api_declares() {
    use split::notes::Notes as _;

    let url = quoll::test::spawn(split_router()).await;
    let api = split::Api::client(&url).expect("the URL is an api's");

    let error = api.notes().get(1).await.unwrap_err();

    assert_eq!(error.status(), Some(StatusCode::UNAUTHORIZED));
}

#[test]
fn a_split_contract_has_the_document_of_the_contract_in_one_block() {
    assert_eq!(
        split::Api::openapi().to_json_pretty(),
        whole::Api::openapi().to_json_pretty()
    );
}

#[tokio::test]
async fn a_group_listed_by_two_apis_is_served_under_each_prefix() {
    use split::notes::Notes as _;

    let router = split::other::Other::router(split::other::OtherServices { notes: App });
    let url = quoll::test::spawn(router).await;
    let seen = Seen::default();
    let other = split::other::Other::client_with(http(&seen), &url).expect("the URL is an api's");

    let note = other.notes().get(1).await.expect("the route is served");
    let missing = split::Api::client(&url)
        .expect("the URL is an api's")
        .notes()
        .get(1)
        .await
        .unwrap_err();

    assert_eq!(note.id, 1);
    assert_eq!(
        missing.status(),
        Some(StatusCode::NOT_FOUND),
        "nothing under /api"
    );
    assert_eq!(
        seen.lock().expect("no test panics with it")[0].3,
        "/other/notes/{id}"
    );

    let document = split::other::Other::openapi().to_json();
    assert!(document.contains(r#""/other/notes/{id}""#), "{document}");
    assert!(!document.contains(r#""/api/notes/{id}""#), "{document}");
}
