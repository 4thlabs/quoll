// Scopes on a route a request may call with no credential.
quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        #[auth(Bearer, optional, scopes("notes:read"))]
        GET "/notes" => list();
    }
}

fn main() {}
