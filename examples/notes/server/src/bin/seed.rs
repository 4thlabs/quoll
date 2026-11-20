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
            // From the environment, which keeps it out of the shell's history.
            let password = std::env::var("NOTES_PASSWORD")
                .context("the password of the user is read from NOTES_PASSWORD")?;
            let user = store.add_user(name, password).await?;

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
