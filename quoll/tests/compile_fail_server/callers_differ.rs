use quoll::rest::{self, Authenticate, Bearer, Response, Result};

rest::api! {
    pub api Api = "/api";

    pub scheme Key = header "X-Api-Key";

    #[auth(Bearer | Key)]
    pub group Admin {
        GET "/stats" => stats() -> String;
    }
}

struct User;
struct Operator;
struct App;

impl Authenticate<Bearer> for App {
    type Caller = User;

    async fn authenticate(&self, _token: String) -> Result<User> {
        Ok(User)
    }
}

// The two schemes of `Bearer | Key` give callers of two types.
impl Authenticate<Key> for App {
    type Caller = Operator;

    async fn authenticate(&self, _key: String) -> Result<Operator> {
        Ok(Operator)
    }
}

impl AdminServer for App {
    async fn stats(&self, _user: &User) -> Result<Response<String>> {
        Ok(Response::new("stats".into()))
    }
}

fn main() {
    let _ = Api::router(ApiServices { admin: App });
}
