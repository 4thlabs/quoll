/// A type of the application's that does not implement `quoll::Scheme`.
pub struct Hmac;

quoll::api! {
    pub api Api = "/api";

    #[auth(Hmac)]
    pub group Admin {
        GET "/stats" => stats();
    }
}

fn main() {}
