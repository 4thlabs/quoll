//! Adds a user or an operator to `notes.db`, in the directory it runs in, which the server serves from.
//!
//! - `seed user <name>` adds a user, whose password is read from `NOTES_PASSWORD`, which keeps it out of the command
//!   line and of the shell's history.
//! - `seed operator <name>` adds an operator, and prints their key, which is shown once: the store keeps its hash.

use anyhow::{Context, bail};
use notes_server::Store;

const USAGE: &str = "usage: seed user <name>, with NOTES_PASSWORD set, or seed operator <name>";

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let arguments: Vec<String> = std::env::args().skip(1).collect();
    let [kind, name] = arguments.as_slice() else {
        bail!(USAGE);
    };
    let store = Store::open("notes.db").await?;

    match kind.as_str() {
        "user" => {
            let password = std::env::var("NOTES_PASSWORD")
                .context("the password of the user is read from NOTES_PASSWORD")?;
            let user = store.add_user(name, &password).await?;

            println!("added the user {}, of id {}", user.name, user.id);
        }
        "operator" => {
            let key = store.add_operator(name).await?;

            println!("added the operator {name}, whose key is shown once: {key}");
        }
        _ => bail!(USAGE),
    }

    Ok(())
}
