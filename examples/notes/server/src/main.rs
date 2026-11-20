//! Serves the notes from `notes.db`, in the directory it runs in, on port 8080, until Ctrl-C, or `SIGTERM` on Unix.
//!
//! The users and the operators are added with the `seed` binary: `NOTES_PASSWORD=... cargo run -p notes-server --bin
//! seed -- user ana`, and `cargo run -p notes-server --bin seed -- operator ops`, which prints the operator's key.

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    tracing_subscriber::fmt::init();

    let app = notes_server::App {
        store: notes_server::Store::open("notes.db").await?,
        sessions: notes_server::Sessions::default(),
    };

    // Until Ctrl-C or SIGTERM, then up to 5 s for the requests it serves.
    notes_server::server()
        .serve("0.0.0.0:8080", notes_server::router(app))
        .await?;
    Ok(())
}
