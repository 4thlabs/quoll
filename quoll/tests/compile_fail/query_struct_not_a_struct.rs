use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes" => search(query filter: Option<NoteFilter>);
    }
}

fn main() {}
