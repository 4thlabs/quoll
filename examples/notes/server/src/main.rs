/// The page `cargo quoll build` bundles, relative to the directory the server runs in: the workspace's.
const PAGE: &str = "web/dist";

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

    // A release serves the page `cargo quoll build` bundled, beside the api on one origin. `cargo quoll dev`
    // leaves the page to Vite, which proxies `/api` here.
    let mut server = notes_server::server();

    if tokio::fs::try_exists(format!("{PAGE}/index.html")).await? {
        println!("the page of {PAGE} at http://localhost:8080/");
        server = server.page(PAGE);
    }

    // Until Ctrl-C or SIGTERM, then up to 5 s for the requests it serves.
    server
        .serve("0.0.0.0:8080", notes_server::router(app))
        .await?;
    Ok(())
}
