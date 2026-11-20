use futures::StreamExt;
use notes_api::{Api, Auth, Login, NewNote, NoteFilter, Notes};
use quoll::client;
use quoll::rest::Form;
use reqwest::StatusCode;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let url = std::env::var("NOTES_URL")?;
    let login = Login {
        user: std::env::var("NOTES_USER")?,
        password: std::env::var("NOTES_PASSWORD")?,
    };
    let token = Api::client(&url)?.auth().token(Form(login)).await?;

    let api = client::builder()
        .with(client::user_agent("notes-cli/0.1"))
        .with(client::request_id())
        .with(client::retry(3))
        .with(client::bearer(token.access_token))
        .build::<Api>(&url)?;

    let created = api
        .notes()
        .create(NewNote {
            title: "Milk".into(),
            body: String::new(),
        })
        .await?;
    let filter = NoteFilter {
        contains: Some("milk".into()),
        ..Default::default()
    };

    for note in api.notes().search(filter).await? {
        println!("{} {}", note.id, note.title);
    }

    match api.notes().get(999).await {
        Err(error) if error.status() == Some(StatusCode::NOT_FOUND) => println!("no note 999"),
        found => println!("{:?}", found?),
    }

    let mut events = api.notes().events(created.id).await?;

    while let Some(event) = events.next().await {
        println!("{:?}", event?);
    }

    Ok(())
}
