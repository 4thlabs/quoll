use std::process::Stdio;
use std::time::Duration;

use axum::extract::Request;
use axum::middleware::{self, Next};
use notes_api::{Api, NewNote, Note, NoteEvent, NoteFilter, Notes};
use quoll::client;
use tokio::process::Command;
use tokio::sync::mpsc;

/// A bound, so that a stream that does not end fails the test rather than hang it.
const BOUND: Duration = Duration::from_secs(60);

#[tokio::test]
async fn the_command_line_signs_in_writes_searches_and_watches() -> anyhow::Result<()> {
    let app = notes_server::App::in_memory();
    let ana = app.store.add_user("ana", "correct horse").await?;
    let store = app.store.clone();
    let sessions = app.sessions.clone();
    let (watched, mut watching) = mpsc::unbounded_channel();
    // The stream of a note is answered once the store watches the note: an update after it reaches the command line.
    let router = notes_server::server()
        .into_router(notes_server::router(app))
        .layer(middleware::from_fn(move |request: Request, next: Next| {
            let watched = watched.clone();

            async move {
                let events = request.uri().path().ends_with("/events");
                let response = next.run(request).await;

                if events {
                    let _ = watched.send(());
                }

                response
            }
        }));
    let url = quoll::test::spawn(router).await;

    let mut cli = Command::new(env!("CARGO_BIN_EXE_notes-cli"))
        .env("NOTES_URL", &url)
        .env("NOTES_USER", "ana")
        .env("NOTES_PASSWORD", "correct horse")
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .kill_on_drop(true)
        .spawn()?;

    let watched = tokio::time::timeout(BOUND, async {
        tokio::select! {
            watched = watching.recv() => Ok(watched),
            ended = cli.wait() => Err(ended),
        }
    })
    .await?;

    if let Err(ended) = watched {
        let output = cli.wait_with_output().await?;

        anyhow::bail!(
            "the command line ended before it watched its note, {ended:?}: {}",
            String::from_utf8_lossy(&output.stderr)
        );
    }

    let [note] = <[Note; 1]>::try_from(store.notes(ana.id, &NoteFilter::default()).await?)
        .map_err(|notes| anyhow::anyhow!("the command line wrote one note, not {notes:?}"))?;
    let api = client::builder()
        .with(client::bearer(sessions.open(ana)))
        .build::<Api>(&url)?;
    let edited = api
        .notes()
        .update(
            note.id,
            NewNote {
                title: "Oat milk".into(),
                body: String::new(),
            },
        )
        .await?;

    api.notes().remove(note.id).await?;

    let output = tokio::time::timeout(BOUND, cli.wait_with_output()).await??;
    let printed = String::from_utf8(output.stdout)?;

    assert!(
        output.status.success(),
        "{}: {}",
        output.status,
        String::from_utf8_lossy(&output.stderr)
    );
    assert_eq!(
        printed.lines().collect::<Vec<_>>(),
        [
            format!("{} Milk", note.id),
            "no note 999".into(),
            format!("{:?}", NoteEvent::Edited(edited)),
            format!("{:?}", NoteEvent::Removed { id: note.id }),
        ]
    );
    Ok(())
}
