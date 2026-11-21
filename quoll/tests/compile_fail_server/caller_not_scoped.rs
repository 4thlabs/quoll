use quoll::rest::{self, Authenticate, Bearer, Response, Result};

rest::api! {
    pub api Api = "/api";

    pub group Repos {
        #[auth(Bearer, scopes("repo"))]
        DELETE "/repos/{id}" => remove(id: i64);
    }
}

// A caller that does not implement `rest::Scoped`, which a route with scopes needs.
struct User;
struct App;

impl Authenticate<Bearer> for App {
    type Caller = User;

    async fn authenticate(&self, _token: String) -> Result<User> {
        Ok(User)
    }
}

impl ReposServer for App {
    async fn remove(&self, _user: &User, _id: i64) -> Result<Response<()>> {
        Ok(Response::new(()))
    }
}

fn main() {
    let _ = Api::router(ApiServices { repos: App });
}
