//! A contract only served, a webhook receiver, built with any features of quoll: no client side, and each import the
//! contract names used.

#![deny(unused_imports)]

use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

// The types need what the server and the document read and write alone: a body `Deserialize`, a result `Serialize`.
#[derive(Debug, Deserialize, JsonSchema)]
pub struct Event {
    pub kind: String,
}

#[derive(Debug, Serialize, JsonSchema)]
pub struct Receipt {
    pub seen: String,
}

mod pings {
    quoll::group! {
        /// A group only served, which the api lists.
        #[client(none)]
        pub group Pings {
            GET "/ping" => ping() -> String;
        }
    }
}

quoll::api! {
    /// Receives the events of another service.
    #[client(none)]
    pub api Hooks = "/hooks" { pings::Pings };

    pub group Events {
        POST "/events" => receive(event: Event) -> Receipt;
    }
}

// The names of the client's items are the application's, whatever the features of quoll: the contract writes none.
#[derive(Debug)]
pub struct EventsClient;

pub trait Events {}

impl Events for EventsClient {}

impl Hooks {
    pub fn client() -> EventsClient {
        EventsClient
    }
}

fn main() {
    let _ = Hooks::openapi();
    let _ = Hooks::client();
}
