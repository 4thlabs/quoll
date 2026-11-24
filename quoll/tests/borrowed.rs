//! Arguments the contract writes `&X`: the client borrows them, the server receives them owned, and the document is
//! the one of the contract written with the owned types. Sent by the client the contract becomes to the server it
//! becomes, served on a port.

#![cfg(all(feature = "server", feature = "client"))]

use std::future::IntoFuture;
use std::num::ParseIntError;

use quoll::{Bytes, Call, Form, Response, Result, Text};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

/// What the server read of a call that lists.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub struct Listed {
    owner: String,
    repo: String,
    labels: Vec<String>,
    state: Option<String>,
    per_page: Option<u32>,
    tags: Vec<String>,
    names: Vec<String>,
}

/// A body, with no `Clone`: the client borrows it and never copies it.
#[derive(Debug, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub struct NewNote {
    title: String,
}

/// A note, as the server answers it.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub struct Note {
    title: String,
    tag: String,
    request_id: Option<String>,
}

/// The `T` of a form.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub struct Entry {
    title: String,
}

/// A query struct.
#[derive(Debug, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub struct Filter {
    q: String,
    page: Option<u32>,
}

/// A body with fixed fields.
#[derive(Debug, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub struct MessageRequest {
    model: String,
}

/// The contract written with `&`.
pub mod borrowed {
    use super::*;

    quoll::api! {
        pub api Api = "/api";

        pub group Repos {
            GET "/repos/{owner}/{repo}/issues?labels={labels}&state={state}&per_page={per_page}&tag={tags}"
                => issues(
                    owner: &str,
                    repo: String,
                    #[join(",")] labels: &[&str],
                    state: Option<&str>,
                    per_page: Option<u32>,
                    tags: &[&str],
                ) -> Listed;
            GET "/names/{owner}?name={names}" => names(owner: &str, names: &[String]) -> Listed;
            POST "/notes"
                => create(header("X-Request-Id") request_id: Option<&str>, header("X-Tag") tag: &str, note: &NewNote) -> Note;
            POST "/form" => form(entry: &Form<Entry>) -> String;
            POST "/form-inner" => form_inner(entry: Form<&Entry>) -> String;
            PUT "/text" => text(text: &Text) -> String;
            PUT "/bytes" => bytes(file: &Bytes) -> usize;
            #[body("stream" = true)]
            POST "/fixed" => fixed(request: &MessageRequest) -> String;
            GET "/search" => search(query filter: &Filter) -> String;
            POST "/many" => many(notes: &[NewNote]) -> usize;
        }
    }

    /// The server of the contract written with `&`, which receives the owned types.
    pub struct App;

    impl ReposServer for App {
        async fn issues(
            &self,
            owner: String,
            repo: String,
            labels: Vec<String>,
            state: Option<String>,
            per_page: Option<u32>,
            tags: Vec<String>,
        ) -> Result<Response<Listed>> {
            Ok(Response::new(Listed {
                owner,
                repo,
                labels,
                state,
                per_page,
                tags,
                names: Vec::new(),
            }))
        }

        async fn names(&self, owner: String, names: Vec<String>) -> Result<Response<Listed>> {
            Ok(Response::new(Listed {
                owner,
                repo: String::new(),
                labels: Vec::new(),
                state: None,
                per_page: None,
                tags: Vec::new(),
                names,
            }))
        }

        async fn create(
            &self,
            request_id: Option<String>,
            tag: String,
            note: NewNote,
        ) -> Result<Response<Note>> {
            Ok(Response::new(Note {
                title: note.title,
                tag,
                request_id,
            }))
        }

        async fn form(&self, entry: Form<Entry>) -> Result<Response<String>> {
            Ok(Response::new(entry.0.title))
        }

        async fn form_inner(&self, entry: Form<Entry>) -> Result<Response<String>> {
            Ok(Response::new(entry.0.title))
        }

        async fn text(&self, text: Text) -> Result<Response<String>> {
            Ok(Response::new(text.0))
        }

        async fn bytes(&self, file: Bytes) -> Result<Response<usize>> {
            Ok(Response::new(file.len()))
        }

        async fn fixed(&self, request: MessageRequest) -> Result<Response<String>> {
            Ok(Response::new(request.model))
        }

        async fn search(&self, filter: Filter) -> Result<Response<String>> {
            Ok(Response::new(format!("{}:{:?}", filter.q, filter.page)))
        }

        async fn many(&self, notes: Vec<NewNote>) -> Result<Response<usize>> {
            Ok(Response::new(notes.len()))
        }
    }
}

/// The same contract written with the owned types, as today.
pub mod owned {
    use super::*;

    quoll::api! {
        pub api Api = "/api";

        pub group Repos {
            GET "/repos/{owner}/{repo}/issues?labels={labels}&state={state}&per_page={per_page}&tag={tags}"
                => issues(
                    owner: String,
                    repo: String,
                    #[join(",")] labels: Vec<String>,
                    state: Option<String>,
                    per_page: Option<u32>,
                    tags: Vec<String>,
                ) -> Listed;
            GET "/names/{owner}?name={names}" => names(owner: String, names: Vec<String>) -> Listed;
            POST "/notes"
                => create(header("X-Request-Id") request_id: Option<String>, header("X-Tag") tag: String, note: NewNote) -> Note;
            POST "/form" => form(entry: Form<Entry>) -> String;
            POST "/form-inner" => form_inner(entry: Form<Entry>) -> String;
            PUT "/text" => text(text: Text) -> String;
            PUT "/bytes" => bytes(file: Bytes) -> usize;
            #[body("stream" = true)]
            POST "/fixed" => fixed(request: MessageRequest) -> String;
            GET "/search" => search(query filter: Filter) -> String;
            POST "/many" => many(notes: Vec<NewNote>) -> usize;
        }
    }
}

use borrowed::{Api, ApiServices, App, Repos};

async fn served() -> borrowed::ApiClient {
    let url = quoll::test::spawn(Api::router(ApiServices { repos: App })).await;

    Api::client(&url).expect("the url is the api's")
}

#[tokio::test]
async fn each_kind_of_argument_is_borrowed_and_received_owned() -> Result<()> {
    let api = served().await;
    let repos = api.repos();

    let owner = String::from("rust-lang");
    let labels = ["C-bug", "T-compiler"];
    let listed = repos
        .issues(
            &owner,
            "rust".into(),
            &labels,
            Some("open"),
            Some(5),
            &["a", "b"],
        )
        .await?;
    assert_eq!(
        listed,
        Listed {
            owner: "rust-lang".into(),
            repo: "rust".into(),
            labels: vec!["C-bug".into(), "T-compiler".into()],
            state: Some("open".into()),
            per_page: Some(5),
            tags: vec!["a".into(), "b".into()],
            names: Vec::new(),
        }
    );

    let names: Vec<String> = vec!["ana".into(), "bo".into()];
    let named = repos.names(&owner, &names).await?;
    assert_eq!(named.names, names);

    let note = NewNote {
        title: "Milk".into(),
    };
    let created = repos.create(Some("r-1"), "shopping", &note).await?;
    assert_eq!(created.title, "Milk");
    assert_eq!(created.tag, "shopping");
    assert_eq!(created.request_id.as_deref(), Some("r-1"));
    // The body is still the caller's after the call: no `Clone`, no move.
    assert_eq!(note.title, "Milk");

    let entry = Entry {
        title: "Bread".into(),
    };
    let form = Form(entry.clone());
    let text = Text::from("hello");
    let bytes = Bytes::new(vec![1, 2, 3]);
    assert_eq!(repos.form(&form).await?, "Bread");
    assert_eq!(repos.form_inner(Form(&entry)).await?, "Bread");
    assert_eq!(repos.text(&text).await?, "hello");
    assert_eq!(repos.bytes(&bytes).await?, 3);
    assert_eq!(bytes.len(), 3);
    assert_eq!(
        repos.fixed(&MessageRequest { model: "m".into() }).await?,
        "m"
    );
    assert_eq!(
        repos
            .search(&Filter {
                q: "x".into(),
                page: Some(2)
            })
            .await?,
        "x:Some(2)"
    );
    assert_eq!(repos.many(&[note, entry_note()]).await?, 2);
    Ok(())
}

fn entry_note() -> NewNote {
    NewNote {
        title: "Tea".into(),
    }
}

#[tokio::test]
async fn the_arguments_are_inferred_as_they_are_written() -> std::result::Result<(), Failure> {
    let api = served().await;
    let repos = api.repos();
    let text = "7";

    let parsed = repos
        .issues("a", "b".into(), &[], Some("x"), Some(text.parse()?), &[])
        .await?;
    let empty = repos
        .issues("a", String::from("b"), &Vec::new(), None, None, &[])
        .await?;
    let none = repos
        .create(
            None,
            Default::default(),
            &serde_json::from_str(r#"{"title":"t"}"#)?,
        )
        .await?;

    assert_eq!(parsed.per_page, Some(7));
    assert_eq!(parsed.state.as_deref(), Some("x"));
    assert!(parsed.labels.is_empty() && parsed.tags.is_empty());
    assert!(empty.labels.is_empty() && empty.state.is_none());
    assert_eq!(none.request_id, None);
    assert_eq!(none.tag, "");
    Ok(())
}

#[tokio::test]
async fn a_call_may_borrow_its_arguments_until_it_is_awaited() -> Result<()> {
    let api = served().await;
    let repos = api.repos();
    let owners: Vec<String> = ["a", "b", "c"].map(str::to_uppercase).into();

    let calls: Vec<_> = owners
        .iter()
        .map(|owner| repos.names(owner, &[]).into_future())
        .collect();
    let listed: Vec<String> = futures_util::future::join_all(calls)
        .await
        .into_iter()
        .map(|listed| listed.map(|listed| listed.owner))
        .collect::<Result<_>>()?;

    assert_eq!(listed, owners);
    Ok(())
}

/// The repositories in memory, in place of the server: its methods take the client's types, and read what they borrow
/// in their `async move`, with no copy before it.
struct Fake;

impl Repos for Fake {
    fn issues(
        &self,
        owner: &str,
        repo: String,
        labels: &[&str],
        state: Option<&str>,
        per_page: Option<u32>,
        tags: &[&str],
    ) -> impl Call<Listed> {
        async move {
            Ok(Listed {
                owner: owner.to_owned(),
                repo,
                labels: labels.iter().map(|label| label.to_string()).collect(),
                state: state.map(str::to_owned),
                per_page,
                tags: tags.iter().map(|tag| tag.to_string()).collect(),
                names: Vec::new(),
            })
        }
    }

    fn names(&self, owner: &str, names: &[String]) -> impl Call<Listed> {
        async move {
            Ok(Listed {
                owner: owner.to_owned(),
                repo: String::new(),
                labels: Vec::new(),
                state: None,
                per_page: None,
                tags: Vec::new(),
                names: names.to_vec(),
            })
        }
    }

    fn create(&self, request_id: Option<&str>, tag: &str, note: &NewNote) -> impl Call<Note> {
        async move {
            Ok(Note {
                title: note.title.clone(),
                tag: tag.to_owned(),
                request_id: request_id.map(str::to_owned),
            })
        }
    }

    fn form(&self, entry: &Form<Entry>) -> impl Call<String> {
        async move { Ok(entry.0.title.clone()) }
    }

    fn form_inner(&self, entry: Form<&Entry>) -> impl Call<String> {
        async move { Ok(entry.0.title.clone()) }
    }

    fn text(&self, text: &Text) -> impl Call<String> {
        async move { Ok(text.0.clone()) }
    }

    fn bytes(&self, file: &Bytes) -> impl Call<usize> {
        async move { Ok(file.len()) }
    }

    fn fixed(&self, request: &MessageRequest) -> impl Call<String> {
        async move { Ok(request.model.clone()) }
    }

    fn search(&self, filter: &Filter) -> impl Call<String> {
        async move { Ok(format!("{}:{:?}", filter.q, filter.page)) }
    }

    fn many(&self, notes: &[NewNote]) -> impl Call<usize> {
        async move { Ok(notes.len()) }
    }
}

/// Code of the application, generic over any client of the group.
async fn title_of(repos: &impl Repos, tag: &str) -> Result<String> {
    let note = NewNote {
        title: format!("for {tag}"),
    };

    Ok(repos.create(None, tag, &note).await?.title)
}

#[tokio::test]
async fn a_fake_takes_the_clients_types() -> Result<()> {
    let tag = String::from("shopping");

    assert_eq!(title_of(&Fake, &tag).await?, "for shopping");
    assert_eq!(
        title_of(&served().await.repos(), &tag).await?,
        "for shopping"
    );
    assert_eq!(Fake.names("ana", &["bo".into()]).await?.names, ["bo"]);
    Ok(())
}

/// A failure of a test that parses its arguments: a call's error, or a parse's.
#[derive(Debug)]
#[expect(dead_code, reason = "read through `Debug` when a test fails")]
enum Failure {
    Call(quoll::Error),
    Parse(ParseIntError),
    Json(serde_json::Error),
}

impl From<quoll::Error> for Failure {
    fn from(error: quoll::Error) -> Self {
        Self::Call(error)
    }
}

impl From<ParseIntError> for Failure {
    fn from(error: ParseIntError) -> Self {
        Self::Parse(error)
    }
}

impl From<serde_json::Error> for Failure {
    fn from(error: serde_json::Error) -> Self {
        Self::Json(error)
    }
}

#[test]
fn the_document_is_the_one_of_the_owned_contract() {
    assert_eq!(
        borrowed::Api::openapi().to_json(),
        owned::Api::openapi().to_json()
    );
}
