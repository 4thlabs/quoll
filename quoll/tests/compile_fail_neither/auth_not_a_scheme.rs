use quoll::rest;

/// A type of the application's that does not implement `rest::Scheme`.
pub struct Hmac;

rest::api! {
    pub api Api = "/api";

    #[auth(Hmac)]
    pub group Admin {
        GET "/stats" => stats();
    }
}

fn main() {}
