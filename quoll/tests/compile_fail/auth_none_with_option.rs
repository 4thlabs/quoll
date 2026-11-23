// `none` takes no option beside it.
quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        #[auth(none, optional)]
        GET "/notes" => list();
    }
}

fn main() {}
