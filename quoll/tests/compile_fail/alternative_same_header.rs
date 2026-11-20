use quoll::rest;

// Two header schemes of an alternative that read one header, whatever its case.
rest::api! {
    pub api Api = "/api";

    pub scheme Key = header "X-Api-Key";

    pub scheme Legacy = header "x-api-key";

    #[auth(Key | Legacy)]
    pub group Admin {
        GET "/stats" => stats();
    }
}

fn main() {}
