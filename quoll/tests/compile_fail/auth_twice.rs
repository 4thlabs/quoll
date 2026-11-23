quoll::api! {
    pub api Api = "/api";

    #[auth(Bearer)]
    pub group Notes {
        #[auth(none)]
        #[auth(Bearer)]
        GET "/notes" => list();
    }
}

fn main() {}
