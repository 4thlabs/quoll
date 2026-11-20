//! Serves the notes from memory on port 8080, until Ctrl-C, or `SIGTERM` on Unix, with one session open, whose token
//! it prints: the store has no login of its own.

use notes_server::User;

#[tokio::main]
async fn main() -> std::io::Result<()> {
    let app = notes_server::App::in_memory();
    let token = app.sessions.open(User {
        id: 1,
        name: "demo".into(),
    });

    println!(
        "serving the notes on http://127.0.0.1:8080, as the user demo with the bearer token {token}"
    );

    notes_server::server()
        .serve("127.0.0.1:8080", notes_server::router(app))
        .await
}
