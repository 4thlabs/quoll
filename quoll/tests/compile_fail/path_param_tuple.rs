// A tuple, which is not one piece of text.
quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: (i64, i64)) -> String;
    }
}

fn main() {}
