// A scheme that is neither the crate's, declared in the contract, nor a type in scope.
quoll::api! {
    pub api Api = "/api";

    #[auth(Key)]
    pub group Admin {
        GET "/stats" => stats();
    }
}

fn main() {}
