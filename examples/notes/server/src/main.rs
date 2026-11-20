//! Serves the notes from memory on port 8080, until Ctrl-C, or `SIGTERM` on Unix.

#[tokio::main]
async fn main() -> std::io::Result<()> {
    let app = notes_server::App::in_memory();

    notes_server::server()
        .serve("127.0.0.1:8080", notes_server::router(app))
        .await
}
