use quoll::rest::{self, Bearer, Route};

rest::api! {
    pub api Api = "/api";

    pub scheme Key = header "X-Api-Key";
    pub scheme Session = cookie "session";
    pub scheme Token = query "token";
    pub scheme Login = openid "https://accounts.example.com/.well-known/openid-configuration";

    #[auth(Bearer | Key | Session | Token)]
    pub group Admin {
        GET "/stats" => stats() -> String;
        #[auth(Login)]
        GET "/login" => login() -> String;
        #[auth(Basic)]
        GET "/basic" => basic() -> String;
    }
}

fn main() -> rest::Result<()> {
    let http = reqwest_middleware::ClientBuilder::new(reqwest::Client::new())
        .with(rest::client::bearer("token"))
        .with(rest::client::basic("ana", "secret"))
        .with(rest::client::key::<Key>("key"))
        .with(rest::client::key::<Session>("session"))
        .with(rest::client::key::<Token>("token"))
        .with(rest::client::key::<Login>("token"))
        .build();
    let _api = Api::client_with(http, "http://localhost:8080")?;

    // A middleware of the application's own tells a route's schemes by their type.
    let takes = |route: &Route| route.takes::<Bearer>() || route.takes::<Key>();
    let _ = takes;
    Ok(())
}
