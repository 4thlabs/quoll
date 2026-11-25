use futures_util::Stream;
use quoll::JsonLines;
use quoll::client::{self, Followed};

quoll::api! {
    pub api Api = "/api";

    pub group Feed {
        GET "/changes?topic={topic}" => changes(topic: &str) -> JsonLines<String>;
    }
}

// An async closure without `move` borrows what it names: the stream cannot outlive it.
fn changes(api: &ApiClient) -> impl Stream<Item = Followed<String>> + 'static {
    let feed = api.feed();
    let topic = "notes".to_owned();

    client::follow(async || feed.changes(&topic).await)
}

fn main() {}
