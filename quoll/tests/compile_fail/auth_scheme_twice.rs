use quoll::rest;

// A scheme written twice in one alternative.
rest::api! {
    pub api Api = "/api";

    #[auth(Bearer | Bearer)]
    pub group Notes {
        GET "/notes" => list();
    }
}

fn main() {}
