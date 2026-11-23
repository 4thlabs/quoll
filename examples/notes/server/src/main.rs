#[tokio::main]
async fn main() -> anyhow::Result<()> {
    tracing_subscriber::fmt::init();

    let app = notes_server::App::in_memory();
    let password = notes_server::random_hex();

    app.store.add_user("demo", &password).await?;

    let key = app.store.add_operator("ops")?;

    println!("throwaway credentials of a demo, made anew at each start:");
    println!("sign in as demo, password {password}, at POST /api/token");
    println!("an operator's X-Api-Key: {key}");

    // A release sets NOTES_PAGE=web/dist, the page `cargo quoll build` bundles, to serve it beside the api on one
    // origin. `cargo quoll dev` does not set it: Vite serves the page there, and proxies `/api` here.
    let mut server = notes_server::server();

    if let Some(page) = std::env::var_os("NOTES_PAGE") {
        println!("the page of {} at http://localhost:8080/", page.display());
        server = server.page(page);
    }

    // Until Ctrl-C or SIGTERM, then up to 5 s for the requests it serves.
    server
        .serve("0.0.0.0:8080", notes_server::router(app))
        .await?;
    Ok(())
}
