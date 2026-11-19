use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        POST "/notes" => create(note: NewNote) -> Created<Note, Extra>;
    }
}

fn main() {}
