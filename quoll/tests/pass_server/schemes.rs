use quoll::rest::{self, Authenticate, Basic, BasicCredential, Bearer, Result, Scoped};

rest::api! {
    pub api Api = "/api";

    pub scheme Key = header "X-Api-Key";

    pub group Admin {
        #[auth(Key | Bearer)]
        GET    "/stats" => stats() -> String;
        #[auth(Bearer | Key, optional)]
        GET    "/me"    => me() -> String;
        #[auth(Bearer, scopes("admin"))]
        DELETE "/stats" => reset();
        #[auth(Basic)]
        GET    "/basic" => basic() -> String;
    }
}

struct User;

impl Scoped for User {
    fn has_scope(&self, scope: &str) -> bool {
        scope == "admin"
    }
}

struct Operator;
struct App;

impl Authenticate<Bearer> for App {
    type Caller = User;

    async fn authenticate(&self, _token: String) -> Result<User> {
        Ok(User)
    }
}

impl Authenticate<Key> for App {
    type Caller = User;

    async fn authenticate(&self, _key: String) -> Result<User> {
        Ok(User)
    }
}

// A scheme in no alternative gives a caller of its own.
impl Authenticate<Basic> for App {
    type Caller = Operator;

    async fn authenticate(&self, _login: BasicCredential) -> Result<Operator> {
        Ok(Operator)
    }
}

impl AdminServer for App {
    async fn stats(&self, _user: &User) -> Result<String> {
        Ok("stats".into())
    }

    async fn me(&self, user: Option<&User>) -> Result<String> {
        Ok(if user.is_some() { "signed in" } else { "anonymous" }.into())
    }

    async fn reset(&self, _user: &User) -> Result<()> {
        Ok(())
    }

    async fn basic(&self, _operator: &Operator) -> Result<String> {
        Ok("basic".into())
    }
}

fn main() {
    let _ = Api::router(ApiServices { admin: App });
}
