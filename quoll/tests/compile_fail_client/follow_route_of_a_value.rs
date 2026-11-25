use quoll::client;

quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

// A route whose result is a value, not a stream: there is nothing to follow.
fn note(api: &ApiClient) {
    let notes = api.notes();

    let _ = client::follow(async move || notes.get(1).await);
}

fn main() {}
