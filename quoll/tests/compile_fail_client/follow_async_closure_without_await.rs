use quoll::JsonLines;
use quoll::client;

quoll::api! {
    pub api Api = "/api";

    pub group Feed {
        GET "/changes" => changes() -> JsonLines<String>;
    }
}

// An async closure that gives the call rather than its answer: the closure gives a result, not a call.
fn changes(api: &ApiClient) {
    let feed = api.feed();

    let _ = client::follow(async move || feed.changes());
}

fn main() {}
