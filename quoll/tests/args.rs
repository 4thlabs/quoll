//! What a client's method takes for each argument, `impl quoll::Arg<T>`: each form a call writes, sent by the client
//! the contract becomes to the server it becomes, served on a port, which answers what it read.

#![cfg(all(feature = "server", feature = "client"))]

use std::cell::Cell;

use quoll::{Response, Result};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
enum State {
    Open,
    Closed,
}

/// What the server read of a call of `options`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
struct Options {
    state: Option<State>,
    page: Option<u32>,
    key: Option<String>,
}

/// What the server read of a call of `numbers`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
struct Numbers {
    id: i64,
    page: u32,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
struct NewNote {
    title: String,
}

/// A body that is `Send` but not `Sync`, which a reference passes all the same, as it is cloned before the call is
/// built.
#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
struct Counter {
    count: Cell<u32>,
}

quoll::api! {
    api Api = "/api";

    group Args {
        GET  "/strings/{owner}/{repo}" => strings(owner: String, repo: String) -> Vec<String>;
        GET  "/lists?labels={labels}&tag={tags}"
            => lists(#[join(",")] labels: Vec<String>, tags: Vec<String>) -> Vec<Vec<String>>;
        GET  "/options?state={state}&page={page}&key={key}"
            => options(state: Option<State>, page: Option<u32>, key: Option<String>) -> Options;
        GET  "/numbers/{id}?page={page}" => numbers(id: i64, page: u32) -> Numbers;
        POST "/notes/{owner}" => create(owner: String, note: NewNote) -> NewNote;
        POST "/counters" => count(counter: Counter) -> u32;
    }
}

struct App;

impl ArgsServer for App {
    async fn strings(&self, owner: String, repo: String) -> Result<Response<Vec<String>>> {
        Ok(Response::new(vec![owner, repo]))
    }

    async fn lists(
        &self,
        labels: Vec<String>,
        tags: Vec<String>,
    ) -> Result<Response<Vec<Vec<String>>>> {
        Ok(Response::new(vec![labels, tags]))
    }

    async fn options(
        &self,
        state: Option<State>,
        page: Option<u32>,
        key: Option<String>,
    ) -> Result<Response<Options>> {
        Ok(Response::new(Options { state, page, key }))
    }

    async fn numbers(&self, id: i64, page: u32) -> Result<Response<Numbers>> {
        Ok(Response::new(Numbers { id, page }))
    }

    async fn count(&self, counter: Counter) -> Result<Response<u32>> {
        Ok(Response::new(counter.count.get()))
    }

    async fn create(&self, owner: String, note: NewNote) -> Result<Response<NewNote>> {
        Ok(Response::new(NewNote {
            title: format!("{owner}: {}", note.title),
        }))
    }
}

/// The api served on a port, and its client.
async fn served() -> ApiClient {
    let url = quoll::test::spawn(Api::router(ApiServices { args: App })).await;

    Api::client(&url).expect("the url is the api's")
}

#[tokio::test]
async fn a_string_takes_a_str_a_reference_or_a_string() -> Result<()> {
    let api = served().await;
    let owner = String::from("rust-lang");
    let repo: &str = "rust";
    let repos = ["rust", "cargo"];

    for name in &repos {
        assert_eq!(
            api.args().strings("rust-lang", name).await?,
            ["rust-lang", name]
        );
    }

    assert_eq!(
        api.args().strings("rust-lang", "rust").await?,
        ["rust-lang", "rust"]
    );
    assert_eq!(
        api.args().strings(&owner, repo).await?,
        ["rust-lang", "rust"]
    );
    assert_eq!(
        api.args()
            .strings(format!("{owner}-x"), owner.clone())
            .await?,
        ["rust-lang-x", "rust-lang"]
    );
    assert_eq!(
        api.args().strings(owner, String::from("cargo")).await?,
        ["rust-lang", "cargo"]
    );
    Ok(())
}

/// Each list as the server answers it.
fn lists(lists: &[&[&str]]) -> Vec<Vec<String>> {
    lists
        .iter()
        .map(|list| list.iter().map(|item| (*item).to_owned()).collect())
        .collect()
}

#[tokio::test]
async fn a_list_takes_an_array_a_slice_a_reference_or_a_vec() -> Result<()> {
    let api = served().await;
    let labels = vec![String::from("C-bug"), String::from("T-compiler")];
    let names = ["C-bug", "T-compiler"];

    assert_eq!(
        api.args().lists(["C-bug", "T-compiler"], ["a"]).await?,
        lists(&[&names, &["a"]])
    );
    assert_eq!(
        api.args().lists(&["C-bug", "T-compiler"], &names).await?,
        lists(&[&names, &names])
    );
    assert_eq!(
        api.args().lists(&labels, &labels[..1]).await?,
        lists(&[&names, &["C-bug"]])
    );
    assert_eq!(
        api.args().lists(&names[..], labels.clone()).await?,
        lists(&[&names, &names])
    );
    assert_eq!(
        api.args().lists(vec![], Vec::new()).await?,
        lists(&[&[], &[]])
    );
    assert_eq!(
        api.args().lists(vec!["C-bug".to_owned()], labels).await?,
        lists(&[&["C-bug"], &names])
    );
    Ok(())
}

#[tokio::test]
async fn an_option_takes_some_or_none() -> Result<()> {
    let api = served().await;
    let key = Some(String::from("order-42"));

    assert_eq!(
        api.args()
            .options(Some(State::Open), Some(5), Some("order-42".into()))
            .await?,
        Options {
            state: Some(State::Open),
            page: Some(5),
            key: Some("order-42".into()),
        }
    );
    assert_eq!(
        api.args().options(None, None, None).await?,
        Options {
            state: None,
            page: None,
            key: None,
        }
    );
    assert_eq!(
        api.args().options(&Some(State::Closed), None, &key).await?,
        Options {
            state: Some(State::Closed),
            page: None,
            key,
        }
    );
    Ok(())
}

#[tokio::test]
async fn a_number_keeps_the_type_of_its_argument() -> Result<()> {
    let api = served().await;

    assert_eq!(
        api.args().numbers(-1, 5).await?,
        Numbers { id: -1, page: 5 }
    );

    for page in 1..3 {
        assert_eq!(api.args().numbers(7, page).await?, Numbers { id: 7, page });
    }

    let parsed = api
        .args()
        .numbers("4".parse::<i64>().expect("a number"), 1)
        .await?;

    assert_eq!(parsed, Numbers { id: 4, page: 1 });

    let id: i64 = 9;

    assert_eq!(
        api.args().numbers(&id, 0).await?,
        Numbers { id: 9, page: 0 }
    );
    Ok(())
}

#[tokio::test]
async fn a_body_takes_its_value_or_a_reference() -> Result<()> {
    let api = served().await;
    let note = NewNote {
        title: "Milk".into(),
    };

    assert_eq!(api.args().create("ana", &note).await?.title, "ana: Milk");
    assert_eq!(api.args().create("bo", note).await?.title, "bo: Milk");
    Ok(())
}

#[tokio::test]
async fn a_body_that_is_not_sync_takes_a_reference() -> Result<()> {
    let api = served().await;
    let counter = Counter {
        count: Cell::new(3),
    };

    assert_eq!(api.args().count(&counter).await?, 3);
    Ok(())
}

/// Code of the application, written for any client of the group, takes the same forms.
async fn labelled(args: &impl Args, owner: &str) -> Result<Vec<String>> {
    let mut lists = args.lists(["C-bug"], [owner]).await?;

    Ok(lists.remove(1))
}

#[tokio::test]
async fn code_written_for_the_trait_takes_the_same_forms() -> Result<()> {
    let api = served().await;

    assert_eq!(labelled(&api.args(), "rust-lang").await?, ["rust-lang"]);
    Ok(())
}
