// A scheme written twice in one alternative.
quoll::api! {
    pub api Api = "/api";

    #[auth(Bearer | Bearer)]
    pub group Notes {
        GET "/notes" => list();
    }
}

fn main() {}
