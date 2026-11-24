quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}"  => get(id: i64) -> String;
        // The client's hidden method of `get` is `__quoll_get`, which would shadow this route.
        GET "/drafts/{id}" => __quoll_get(id: i64) -> String;
    }
}

fn main() {}
