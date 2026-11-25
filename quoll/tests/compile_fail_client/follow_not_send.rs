use std::rc::Rc;

use futures_util::StreamExt;
use quoll::JsonLines;
use quoll::client::{self, Followed};

quoll::api! {
    pub api Api = "/api";

    pub group Feed {
        GET "/changes" => changes() -> JsonLines<String>;
    }
}

// A closure that holds what is not `Send`: the stream is not either, and no task takes it.
fn changes(api: &ApiClient) {
    let feed = Rc::new(api.feed());

    tokio::spawn(async move {
        let mut changes = client::follow(async move || feed.changes().await);

        while let Some(Followed::Item(_)) = changes.next().await {}
    });
}

fn main() {}
