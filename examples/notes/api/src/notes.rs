use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub struct Note {
    pub id: i64,
    pub title: String,
    pub body: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub struct NewNote {
    pub title: String,
    pub body: String,
}

pub const CONTAINS_LIMIT: usize = 1000;

/// Each key given, and not empty, narrows the search.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub struct NoteFilter {
    /// Notes whose title or text holds this, whatever the case of its ASCII letters: 1000 characters at most.
    #[schemars(length(max = CONTAINS_LIMIT))]
    pub contains: Option<String>,
    /// Notes tagged so, by a `#word` of their title or their text: `#milk` or `milk,` is `milk`.
    pub tag: Option<String>,
    /// The page, of 20 notes in the order they were written, from 1, the first when it is missing.
    pub page: Option<u32>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum NoteEvent {
    Edited(Note),
    /// The last event of the note's stream.
    Removed {
        id: i64,
    },
}
