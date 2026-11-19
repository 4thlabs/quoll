use quoll::rest;

// An argument that reads the header of a scheme the route takes.
rest::api! {
    pub api Api = "/api";

    pub scheme Key = header "X-Api-Key";

    #[auth(Key)]
    pub group Admin {
        GET "/stats" => stats(header("x-api-key") key: String);
    }
}

fn main() {}
