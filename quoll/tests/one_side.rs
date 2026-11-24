//! Contracts with one side, each on a port: one only served, `#[client(none)]`, called over HTTP by one only called,
//! `#[server(none)]`, of the same routes; and a group of both sides, which an api of each side lists. The types of the
//! webhook, which has no document, and of its caller derive only what their side reads and writes.

#![cfg(all(feature = "server", feature = "client"))]

use quoll::{Response, Result};

use crate::both::things::Things as _;
use crate::sender::Events as _;

/// A webhook receiver, which the application only serves.
mod receiver {
    use serde::{Deserialize, Serialize};

    /// An event, which the server only reads.
    #[derive(Debug, Deserialize)]
    pub struct Event {
        pub kind: String,
    }

    /// The receipt of an event, which the server only writes.
    #[derive(Debug, Serialize)]
    pub struct Receipt {
        pub seen: String,
    }

    quoll::api! {
        /// Receives the events of another service, with no document.
        #[client(none)]
        #[openapi(none)]
        pub api Hooks = "/hooks";

        pub group Events {
            POST "/events" => receive(event: Event) -> Receipt;
        }
    }
}

/// The service that sends the events, which the application only calls.
mod sender {
    use serde::{Deserialize, Serialize};

    /// An event, which the client only writes.
    #[derive(Debug, Serialize)]
    pub struct Event {
        pub kind: String,
    }

    /// The receipt of an event, which the client only reads.
    #[derive(Debug, Deserialize)]
    pub struct Receipt {
        pub seen: String,
    }

    quoll::api! {
        /// Sends events to a receiver.
        #[server(none)]
        pub api Sender = "/hooks";

        pub group Events {
            POST "/events" => receive(event: &Event) -> Receipt;
        }
    }
}

/// A group of both sides, which an api only served and an api only called both list.
mod both {
    pub mod things {
        quoll::group! {
            pub group Things {
                GET "/things/{id}" => get(id: i64) -> String;
            }
        }
    }

    quoll::api! {
        #[client(none)]
        pub api Served = "/v1" { things::Things };
    }

    quoll::api! {
        #[server(none)]
        pub api Called = "/v1" { things::Things };
    }
}

#[derive(Clone)]
struct App;

impl receiver::EventsServer for App {
    async fn receive(&self, event: receiver::Event) -> Result<Response<receiver::Receipt>> {
        Ok(Response::new(receiver::Receipt { seen: event.kind }))
    }
}

impl both::things::ThingsServer for App {
    async fn get(&self, id: i64) -> Result<Response<String>> {
        Ok(Response::new(format!("thing {id}")))
    }
}

#[tokio::test]
async fn a_contract_only_called_calls_one_only_served() {
    let router = receiver::Hooks::router(receiver::HooksServices { events: App });
    let url = quoll::test::spawn(router).await;
    let sender = sender::Sender::client(&url).expect("the URL is valid");
    let event = sender::Event {
        kind: "push".into(),
    };

    let receipt = sender
        .events()
        .receive(&event)
        .await
        .expect("the receiver answers");

    assert_eq!(receipt.seen, "push");
}

#[tokio::test]
async fn a_group_of_both_sides_is_served_by_one_api_and_called_by_another() {
    let router = both::Served::router(both::ServedServices { things: App });
    let url = quoll::test::spawn(router).await;
    let called = both::Called::client(&url).expect("the URL is valid");

    let thing = called.things().get(7).await.expect("the server answers");

    assert_eq!(thing, "thing 7");
}
