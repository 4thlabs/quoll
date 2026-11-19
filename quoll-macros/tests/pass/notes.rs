use quoll::rest;

rest::api! {
    /// Notes, kept per user.
    pub api Api = "/api";

    pub group Auth {
        /// Trades a login for a token.
        POST "/token" => token(login: Login) -> TokenResponse;
    }

    /// The signed-in user's notes.
    pub group Notes {
        GET    "/notes?contains={contains}&page={page}"
               => list(contains: Option<String>, page: Option<u32>) -> Vec<Note>;
        GET    "/notes/{id}"        => get(id: i64) -> Note;
        POST   "/notes"             => create(note: NewNote) -> Created<Note>;
        PUT    "/notes/{id}"        => update(id: i64, note: NewNote) -> Note;
        DELETE "/notes/{id}"        => remove(id: i64);
    }
}

struct Login;
struct TokenResponse;
struct Note;
struct NewNote;

fn main() {
    // The api is a type with the comments of the contract; what else it holds comes with the generators.
    let _api = Api;
}
