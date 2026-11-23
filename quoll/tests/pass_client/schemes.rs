use quoll::{Bearer, Route};

quoll::api! {
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

fn main() -> quoll::Result<()> {
    let http = reqwest_middleware::ClientBuilder::new(reqwest::Client::new())
        .with(quoll::client::bearer("token"))
        .with(quoll::client::basic("ana", "secret"))
        .with(quoll::client::key::<Key>("key"))
        .with(quoll::client::key::<Session>("session"))
        .with(quoll::client::key::<Token>("token"))
        .with(quoll::client::key::<Login>("token"))
        .build();
    let _api = Api::client_with(http, "http://localhost:8080")?;

    // A middleware of the application's own tells a route's schemes by their type.
    let takes = |route: &Route| route.takes::<Bearer>() || route.takes::<Key>();
    let _ = takes;
    Ok(())
}
