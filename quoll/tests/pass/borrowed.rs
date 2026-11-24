use quoll::{Bytes, Form, Text};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, JsonSchema)]
pub struct Note {
    pub title: String,
}

#[derive(Serialize, Deserialize, JsonSchema)]
pub struct NoteFilter {
    pub contains: Option<String>,
}

// A `&` makes the client borrow an argument, whatever the features of quoll: the server, when there is one, and the
// document receive the owned type. `&'static str` is borrowed for as long as it lives, as before.
quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET  "/notes/{owner}/{repo}?tag={tags}&state={state}"
            => list(owner: &str, repo: &'static str, #[join(",")] tags: &[&str], state: Option<&str>) -> Vec<Note>;
        GET  "/search" => search(query filter: &NoteFilter, header("If-Match") version: &str) -> Vec<Note>;
        POST "/notes" => create(note: &Note) -> Note;
        POST "/many" => many(notes: &[Note], header("X-Tags") tag: Option<&'static str>);
        POST "/form" => form(note: &Form<Note>);
        PUT  "/text" => text(text: &Text);
        PUT  "/file" => upload(file: &Bytes);
        GET  "/names?name={names}&other={others}" => names(names: &[String], others: Vec<&str>) -> Vec<String>;
        GET  "/fixed?name={names}&tag={tags}"
            => fixed(names: &'static [&str], tags: Option<&'static [&'static str]>) -> Vec<String>;
    }
}

// A type handed over by a `macro_rules!` arrives in an invisible group: `&$text` is still a `&str`, received as a
// `String`, and `&$path` too, `std::primitive::str` being `str`.
macro_rules! borrowed {
    ($text:ty, $path:ty) => {
        mod grouped {
            quoll::api! {
                pub api Api = "/api";

                pub group Grouped {
                    GET "/grouped/{owner}?name={name}"
                        => get(owner: &$text, name: &$path) -> String;
                }
            }
        }
    };
}

borrowed!(str, std::primitive::str);

fn main() {}
