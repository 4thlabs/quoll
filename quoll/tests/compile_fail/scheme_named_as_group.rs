use quoll::rest;

// A scheme named as a group, which both become types.
rest::api! {
    pub api Api = "/api";

    pub scheme Admin = header "X-Api-Key";

    #[auth(Admin)]
    pub group Admin {
        GET "/stats" => stats();
    }
}

fn main() {}
