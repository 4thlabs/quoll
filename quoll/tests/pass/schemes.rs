use http::request::Parts;
use quoll::{Result, Scheme, openapi::SecurityScheme};

/// A scheme of the application's own.
pub struct Hmac;

impl Scheme for Hmac {
    type Credential = String;

    const NAME: &'static str = "Hmac";

    fn read(request: &Parts) -> Result<Option<String>> {
        Ok(request
            .headers
            .get("x-signature")
            .and_then(|value| value.to_str().ok())
            .map(str::to_owned))
    }

    fn openapi() -> SecurityScheme {
        SecurityScheme::header("X-Signature")
    }
}

mod auth {
    pub use super::Hmac;
}

// Whatever the features, a contract with every kind of scheme compiles: each side writes what it needs of it.
quoll::api! {
    pub api Api = "/api";

    /// The key of an operator's scripts.
    pub scheme Key = header "X-Api-Key";
    pub scheme Session = cookie "session";
    pub scheme Token = query "token";
    pub scheme Login = openid "https://accounts.example.com/.well-known/openid-configuration";

    #[auth(Bearer | Key)]
    pub group Admin {
        GET    "/stats"     => stats() -> String;
        #[auth(Basic)]
        GET    "/basic"     => basic() -> String;
        #[auth(Session, optional)]
        GET    "/me"        => me() -> String;
        #[auth(Key | Bearer | crate::auth::Hmac, scopes("admin", "stats:read"))]
        DELETE "/stats"     => reset();
        #[auth(Login)]
        GET    "/login"     => login() -> String;
        #[auth(none)]
        GET    "/health"    => health();
    }

    pub group Internal {
        #[auth(Token)]
        GET "/internal" => internal() -> String;
    }
}

fn main() {
    let _ = Key;
    assert_eq!(<Key as Scheme>::NAME, "Key");
    assert_eq!(<Login as Scheme>::CHALLENGE, "Bearer");
    assert_eq!(<Session as Scheme>::CHALLENGE, "Session");
}
