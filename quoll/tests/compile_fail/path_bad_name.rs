quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{note-id}" => get(note_id: i64);
    }
}

fn main() {}
