use quoll::rest::{self, Created, Events, Form};

use crate::{Login, NewNote, Note, NoteEvent, NoteFilter, Stats, TokenResponse};

rest::api! {
    /// Notes, kept per user.
    pub api Api = "/api";

    pub scheme Key = header "X-Api-Key";

    pub group Auth {
        /// Trades a login for a token.
        POST "/token" => token(login: Form<Login>) -> TokenResponse;
    }

    #[auth(Bearer)]
    pub group Notes {
        GET    "/notes"             => search(query filter: NoteFilter) -> Vec<Note>;
        GET    "/notes/{id}"        => get(id: i64) -> Note;
        POST   "/notes"             => create(note: NewNote) -> Created<Note>;
        PUT    "/notes/{id}"        => update(id: i64, note: NewNote) -> Note;
        DELETE "/notes/{id}"        => remove(id: i64);
        GET    "/notes/{id}/events" => events(id: i64) -> Events<NoteEvent>;
    }

    #[auth(Key)]
    pub group Admin {
        GET "/admin/stats" => stats() -> Stats;
    }
}
