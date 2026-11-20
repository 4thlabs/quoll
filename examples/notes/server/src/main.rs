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

    // Until Ctrl-C or SIGTERM, then up to 5 s for the requests it serves.
    notes_server::server()
        .serve("0.0.0.0:8080", notes_server::router(app))
        .await?;
    Ok(())
}
