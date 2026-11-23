quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        POST "/notes" => create(note: NewNote) -> Created;
    }
}

fn main() {}
