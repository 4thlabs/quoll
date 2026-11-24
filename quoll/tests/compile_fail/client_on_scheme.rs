quoll::api! {
    pub api Api = "/api";

    #[client(none)]
    pub scheme Key = header "X-Api-Key";
}

fn main() {}
