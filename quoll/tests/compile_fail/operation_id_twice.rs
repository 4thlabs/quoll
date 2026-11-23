quoll::api! {
    pub api Api = "/api";

    pub group Note {
        GET "/notes/tags" => tags_list() -> Vec<String>;
    }

    pub group NoteTags {
        GET "/tags" => list() -> Vec<String>;
    }
}

fn main() {}
