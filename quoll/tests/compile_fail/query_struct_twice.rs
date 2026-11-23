quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes" => search(query filter: NoteFilter, query paging: Paging);
    }
}

fn main() {}
