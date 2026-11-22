use quoll::rest;

// A query key that a scheme of the route reads.
rest::api! {
    pub api Api = "/api";

    pub scheme Token = query "token";

    #[auth(Token)]
    pub group Notes {
        GET "/notes?token={token}" => list(token: String);
    }
}

fn main() {}
