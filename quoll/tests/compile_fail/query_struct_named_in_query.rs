use quoll::rest;

// The query struct named by a key of the string's query too.
rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes?page={page}" => list(query page: Paging);
    }
}

fn main() {}
