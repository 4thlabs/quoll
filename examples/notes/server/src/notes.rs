use notes_api::{CONTAINS_LIMIT, NewNote, Note, NoteEvent, NoteFilter, NotesServer};
use quoll::rest::{Created, Error, Events, Result};

use crate::{App, User};

impl NotesServer for App {
    async fn search(&self, user: &User, filter: NoteFilter) -> Result<Vec<Note>> {
        // Refused before the query, so that a pattern past SQLite's limit is the caller's error, not the database's.
        if filter
            .contains
            .as_ref()
            .is_some_and(|text| text.chars().count() > CONTAINS_LIMIT)
        {
            return Err(Error::bad_request(format!(
                "`contains` holds more than {CONTAINS_LIMIT} characters"
            )));
        }

        self.store
            .notes(user.id, &filter)
            .await
            .map_err(Error::internal)
    }

    async fn get(&self, user: &User, id: i64) -> Result<Note> {
        self.store
            .note(user.id, id)
            .await
            .map_err(Error::internal)?
            .ok_or_else(|| Error::not_found("no such note"))
    }

    async fn create(&self, user: &User, note: NewNote) -> Result<Created<Note>> {
        let note = self
            .store
            .add(user.id, note)
            .await
            .map_err(Error::internal)?;

        Ok(Created::at(format!("/api/notes/{}", note.id), note))
    }

    async fn update(&self, user: &User, id: i64, note: NewNote) -> Result<Note> {
        self.store
            .replace(user.id, id, note)
            .await
            .map_err(Error::internal)?
            .ok_or_else(|| Error::not_found("no such note"))
    }

    async fn remove(&self, user: &User, id: i64) -> Result<()> {
        self.store
            .remove(user.id, id)
            .await
            .map_err(Error::internal)
    }

    async fn events(&self, user: &User, id: i64) -> Result<Events<NoteEvent>> {
        let changes = self
            .store
            .watch(user.id, id)
            .await
            .map_err(Error::internal)?
            .ok_or_else(|| Error::not_found("no such note"))?;

        Ok(Events::new(changes))
    }
}
