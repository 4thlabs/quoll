use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes?page={page}" => search(page: u32, query filter: NoteFilter);
    }
}

fn main() {}
