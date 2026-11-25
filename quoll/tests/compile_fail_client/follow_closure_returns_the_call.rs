use quoll::JsonLines;
use quoll::client;

quoll::api! {
    pub api Api = "/api";

    pub group Feed {
        GET "/changes?topic={topic}" => changes(topic: &str) -> JsonLines<String>;
    }
}

// `follow` takes a closure that makes the call and awaits it: a call returned as it is, which borrows the group's
// client and the topic that the closure holds, is not a future.
fn changes(api: &ApiClient, topic: String) {
    let feed = api.feed();

    let _ = client::follow(move || feed.changes(&topic));
}

fn main() {}
