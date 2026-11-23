use axum::http::header::LOCATION;
use notes_api::{CONTAINS_LIMIT, NewNote, Note, NoteEvent, NoteFilter, NotesServer};
use quoll::{Error, Events, Response, Result};

use crate::{App, User};

impl NotesServer for App {
    async fn search(&self, user: &User, filter: NoteFilter) -> Result<Response<Vec<Note>>> {
        // The document says 1000 at most: a caller who skips its check meets the same rule, and a search under the
        // store's lock stays short.
        if filter
            .contains
            .as_ref()
            .is_some_and(|text| text.chars().count() > CONTAINS_LIMIT)
        {
            return Err(Error::bad_request(format!(
                "`contains` holds more than {CONTAINS_LIMIT} characters"
            )));
        }

        Ok(Response::new(self.store.notes(user.id, &filter)))
    }

    async fn get(&self, user: &User, id: i64) -> Result<Response<Note>> {
        self.store
            .note(user.id, id)
            .map(Response::new)
            .ok_or_else(|| Error::not_found("no such note"))
    }

    async fn create(&self, user: &User, note: NewNote) -> Result<Response<Note>> {
        let note = self.store.add(user.id, note);
        let location = format!("/api/notes/{}", note.id);

        Ok(Response::new(note).header(LOCATION, location))
    }

    async fn update(&self, user: &User, id: i64, note: NewNote) -> Result<Response<Note>> {
        self.store
            .replace(user.id, id, note)
            .map(Response::new)
            .ok_or_else(|| Error::not_found("no such note"))
    }

    async fn remove(&self, user: &User, id: i64) -> Result<Response<()>> {
        self.store.remove(user.id, id);
        Ok(Response::new(()))
    }

    async fn events(&self, user: &User, id: i64) -> Result<Response<Events<NoteEvent>>> {
        let changes = self
            .store
            .watch(user.id, id)
            .ok_or_else(|| Error::not_found("no such note"))?;

        Ok(Response::new(Events::new(changes)))
    }
}
