use quoll::rest;

// `none` takes no scheme beside it.
rest::api! {
    pub api Api = "/api";

    pub group Notes {
        #[auth(none | Bearer)]
        GET "/notes" => list();
    }
}

fn main() {}
