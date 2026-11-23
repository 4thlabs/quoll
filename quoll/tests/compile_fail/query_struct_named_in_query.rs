// The query struct named by a key of the string's query too.
quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes?page={page}" => list(query page: Paging);
    }
}

fn main() {}
