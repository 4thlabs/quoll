use quoll::{Arg, Call, Result};

quoll::api! {
    pub api Api = "/api";

    #[auth(Bearer)]
    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
        #[auth(none)]
        GET "/public" => public() -> String;
    }
}

// The client's trait has no caller: its middleware sends the credential.
struct Fake;

impl Notes for Fake {
    fn get(&self, id: impl Arg<i64>) -> impl Call<String> {
        let id = id.into_value();

        async move {
            Ok(id.to_string())
        }
    }

    fn public(&self) -> impl Call<String> {
        async move {
            Ok("open".into())
        }
    }
}

fn main() -> Result<()> {
    let http = reqwest_middleware::ClientBuilder::new(reqwest::Client::new())
        .with(quoll::client::bearer("a-token"))
        .build();
    let _api: ApiClient = Api::client_with(http, "http://localhost:8080")?;
    let _fake = Fake;

    Ok(())
}
