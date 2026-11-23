quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        PUT "/notes/{id}" => update(id: i64, header("content-type") kind: String, note: NewNote);
    }
}

fn main() {}
