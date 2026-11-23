use quoll::rest::{self, Created};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Debug, Serialize, Deserialize, JsonSchema)]
pub struct Note {
    pub title: String,
}

rest::api! {
    #[openapi(none)]
    pub api Api = "";

    pub group Notes {
        // Its handler would answer a `Response<Option<Note>>`, which a 204 could be given: no answer of a 201.
        POST "/notes" => create(title: String) -> Created<Option<Note>>;
    }
}

fn main() {}
