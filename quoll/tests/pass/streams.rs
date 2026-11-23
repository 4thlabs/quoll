use quoll::{ByteStream, Events, JsonLines};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, JsonSchema)]
pub struct Change {
    pub id: i64,
}

// The streams the contract names by their name are the user's imports, used whatever the features of quoll.
quoll::api! {
    pub api Api = "/api";

    pub group Feeds {
        GET  "/events"         => events() -> Events<Change>;
        GET  "/notes/{id}/events"
             => note_events(id: i64, header("Last-Event-ID") last: Option<String>) -> quoll::Events<Change>;
        GET  "/export"         => export() -> JsonLines<Change>;
        POST "/import"         => import(changes: Vec<Change>) -> quoll::JsonLines<Change>;
        GET  "/logs/{id}?follow={follow}"
             => logs(id: i64, follow: Option<bool>) -> ByteStream;
        POST "/run"            => run(script: quoll::Text) -> quoll::ByteStream;
    }
}

fn main() {}
