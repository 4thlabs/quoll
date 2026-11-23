// A scheme named as a group, which both become types.
quoll::api! {
    pub api Api = "/api";

    pub scheme Admin = header "X-Api-Key";

    #[auth(Admin)]
    pub group Admin {
        GET "/stats" => stats();
    }
}

fn main() {}
