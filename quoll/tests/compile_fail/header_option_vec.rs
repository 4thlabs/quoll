// An `Option` of a type that is not read from text either.
quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes" => list(header("X-Tags") tags: Option<Vec<String>>);
    }
}

fn main() {}
