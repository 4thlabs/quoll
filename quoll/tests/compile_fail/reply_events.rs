use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes" => watch() -> Events<NoteEvent>;
    }
}

fn main() {}
