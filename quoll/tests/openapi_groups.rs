//! The OpenAPI document of an api that lists groups of other blocks, whatever the features: the schemes of those
//! groups, which the macro does not see, found among the api's by their names as the document is built.

use http::request::Parts;
use quoll::rest::openapi::SecurityScheme;
use quoll::rest::{self, Result, Scheme};
use serde_json::Value;

/// A scheme of the application's own, which takes the name of the api's declared `Key`.
pub struct Impostor;

impl Scheme for Impostor {
    type Credential = String;

    const NAME: &'static str = "Key";

    fn read(_request: &Parts) -> Result<Option<String>> {
        Ok(None)
    }

    fn openapi() -> SecurityScheme {
        SecurityScheme::header("X-Impostor")
    }
}

mod notes {
    use quoll::rest;

    rest::group! {
        /// Notes, kept in a session.
        #[auth(crate::Session)]
        pub group Notes {
            POST "/notes" => create();
            GET  "/notes" => list() -> Vec<String>;
        }
    }
}

mod impostor {
    use quoll::rest;

    rest::group! {
        #[auth(crate::Impostor)]
        pub group Stats {
            GET "/stats" => stats() -> u64;
        }
    }
}

rest::api! {
    pub api Api = "/api" { notes::Notes };

    /// The session of the web page.
    pub scheme Session = cookie "session";
}

/// An api that lists the group of notes and declares no scheme: the group's cookie scheme is the first api's.
mod mirror {
    use quoll::rest;

    rest::api! {
        pub api Mirror = "/mirror" { crate::notes::Notes };
    }
}

mod clash {
    use quoll::rest;

    rest::api! {
        pub api Clash = "/api" { crate::impostor::Stats };

        pub scheme Key = header "X-Api-Key";

        #[auth(Key)]
        pub group Admin {
            GET "/admin" => admin() -> u64;
        }
    }
}

#[test]
fn a_cookie_scheme_the_api_declares_answers_403_on_the_routes_of_a_listed_group() {
    let document: Value =
        serde_json::from_str(&Api::openapi().to_json()).expect("the document is JSON");
    let notes = &document["paths"]["/api/notes"];

    assert!(notes["post"]["responses"]["403"].is_object(), "{notes}");
    assert!(notes["get"]["responses"]["403"].is_null(), "{notes}");
    assert_eq!(
        document["components"]["securitySchemes"]["Session"]["in"], "cookie",
        "{document}"
    );
    assert_eq!(
        notes["post"]["security"][0]["Session"],
        serde_json::json!([])
    );
}

#[test]
fn a_cookie_scheme_answers_403_in_the_document_of_an_api_that_does_not_declare_it() {
    let document: Value =
        serde_json::from_str(&mirror::Mirror::openapi().to_json()).expect("the document is JSON");
    let notes = &document["paths"]["/mirror/notes"];

    assert!(notes["post"]["responses"]["403"].is_object(), "{notes}");
    assert!(notes["get"]["responses"]["403"].is_null(), "{notes}");
}

#[test]
#[should_panic(
    expected = "the schemes `openapi_groups::clash::Key` and `openapi_groups::Impostor` of the api `Clash` have one `NAME`, `Key`, which names a scheme in the OpenAPI document: give each scheme its own"
)]
fn two_schemes_of_one_name_from_two_blocks_fail_the_document() {
    clash::Clash::openapi();
}
