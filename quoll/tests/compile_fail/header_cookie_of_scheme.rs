// An argument that reads the cookies, one of which a scheme of the route reads.
quoll::api! {
    pub api Api = "/api";

    pub scheme Session = cookie "session";

    #[auth(Session)]
    pub group Notes {
        GET "/notes" => list(header("Cookie") cookies: String);
    }
}

fn main() {}
