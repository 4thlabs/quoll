//! A group's trait implemented by a fake, which code written for the client takes as well.

#![cfg(feature = "client")]

use std::collections::BTreeMap;

use quoll::{Call, Created, Error, Result};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
struct Note {
    id: i64,
    title: String,
}

#[derive(Debug, Serialize, Deserialize, JsonSchema)]
struct NewNote {
    title: String,
}

quoll::api! {
    api Api = "/api";

    group Notes {
        GET    "/notes/{id}" => get(id: i64) -> Note;
        POST   "/notes"      => create(note: NewNote) -> Created<Note>;
        DELETE "/notes/{id}" => remove(id: i64);
    }
}

/// Notes kept in memory, in place of a server.
#[derive(Default)]
struct Fake {
    notes: std::sync::Mutex<BTreeMap<i64, Note>>,
}

impl Notes for Fake {
    fn get(&self, id: i64) -> impl Call<Note> {
        async move {
            let notes = self.notes.lock().expect("no test panics while holding it");

            notes
                .get(&id)
                .cloned()
                .ok_or_else(|| Error::not_found("no such note"))
        }
    }

    fn create(&self, note: NewNote) -> impl Call<Note> {
        async move {
            let mut notes = self.notes.lock().expect("no test panics while holding it");
            let id = i64::try_from(notes.len()).expect("few notes") + 1;
            let note = Note {
                id,
                title: note.title,
            };
            notes.insert(id, note.clone());

            Ok(note)
        }
    }

    fn remove(&self, id: i64) -> impl Call<()> {
        async move {
            let mut notes = self.notes.lock().expect("no test panics while holding it");

            notes
                .remove(&id)
                .map(|_| ())
                .ok_or_else(|| Error::not_found("no such note"))
        }
    }
}

/// Code of the application, written for any client of the group.
async fn copy(notes: &impl Notes, id: i64) -> Result<Note> {
    let note = notes.get(id).await?;

    notes
        .create(NewNote {
            title: format!("{} (copy)", note.title),
        })
        .await
}

#[tokio::test]
async fn a_fake_stands_for_the_client() -> Result<()> {
    let fake = Fake::default();

    fake.create(NewNote {
        title: "Milk".into(),
    })
    .await?;
    let copied = copy(&fake, 1).await?;

    assert_eq!(
        copied,
        Note {
            id: 2,
            title: "Milk (copy)".into()
        }
    );
    assert_eq!(
        copy(&fake, 9).await.expect_err("no note 9").status(),
        Some(http::StatusCode::NOT_FOUND)
    );
    fake.remove(1).await?;
    Ok(())
}

#[test]
fn code_written_for_the_trait_is_send() {
    fn is_send<T: Send>(_: &T) {}

    let fake = Fake::default();
    let future = copy(&fake, 1);

    is_send(&future);
}

#[test]
fn the_client_and_the_fake_are_one_trait() {
    fn takes<T: Notes>() {}

    takes::<Fake>();
    takes::<NotesClient>();
}

#[test]
fn the_api_client_is_cheap_to_clone_and_sendable() {
    fn is_shared<T: Clone + Send + Sync + std::fmt::Debug + 'static>() {}

    is_shared::<ApiClient>();
    is_shared::<NotesClient>();

    let api = Api::client("http://localhost:8080").expect("a valid url");
    let _notes: NotesClient = api.clone().notes();
}
