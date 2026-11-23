// `none` takes no scheme beside it.
quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        #[auth(none | Bearer)]
        GET "/notes" => list();
    }
}

fn main() {}
