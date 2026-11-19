use quoll::rest;

// A scope with a space, which OAuth separates scopes with.
rest::api! {
    pub api Api = "/api";

    pub group Notes {
        #[auth(Bearer, scopes("notes read"))]
        GET "/notes" => list();
    }
}

fn main() {}
