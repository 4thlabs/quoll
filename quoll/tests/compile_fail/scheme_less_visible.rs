// A scheme less visible than a group that takes it, whose server trait names it.
quoll::api! {
    pub api Api = "/api";

    scheme Key = header "X-Api-Key";

    #[auth(Key)]
    pub group Admin {
        GET "/stats" => stats();
    }
}

fn main() {}
