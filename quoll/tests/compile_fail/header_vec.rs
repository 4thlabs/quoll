quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes" => list(header("X-Tags") tags: Vec<String>);
    }
}

fn main() {}
