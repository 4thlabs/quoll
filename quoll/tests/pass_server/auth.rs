use quoll::rest::{self, Authenticate, Bearer, Error, Response, Result};

rest::api! {
    pub api Api = "/api";

    pub group Auth {
        POST "/token" => token(login: String) -> String;
        #[auth(Bearer)]
        DELETE "/token" => revoke();
    }

    #[auth(Bearer)]
    pub group Notes {
        // An argument may take the name the generated code gives the caller.
        GET "/notes/{id}?caller={caller}" => get(id: i64, caller: Option<String>) -> String;
        #[auth(none)]
        GET "/public" => public() -> String;
    }
}

/// The caller, which the contract never names: the server alone says what it is.
struct User {
    name: String,
}

#[derive(Clone)]
struct App;

impl Authenticate<Bearer> for App {
    type Caller = User;

    async fn authenticate(&self, token: String) -> Result<User> {
        if token.is_empty() {
            return Err(Error::unauthorized("unknown token"));
        }

        Ok(User { name: token })
    }
}

impl AuthServer for App {
    async fn token(&self, login: String) -> Result<Response<String>> {
        Ok(Response::new(login))
    }

    async fn revoke(&self, _user: &User) -> Result<Response<()>> {
        Ok(Response::new(()))
    }
}

impl NotesServer for App {
    async fn get(&self, user: &User, id: i64, caller: Option<String>) -> Result<Response<String>> {
        Ok(Response::new(format!("{} {id} {caller:?}", user.name)))
    }

    async fn public(&self) -> Result<Response<String>> {
        Ok(Response::new("open".into()))
    }
}

fn main() {
    let _router: axum::Router = Api::router(ApiServices {
        auth: App,
        notes: App,
    });
}
