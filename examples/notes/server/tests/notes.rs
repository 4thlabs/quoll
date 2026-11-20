use std::time::Duration;

use futures_util::StreamExt;
use notes_api::{
    Admin, Api, ApiClient, Auth, CONTAINS_LIMIT, Key, Login, NewNote, Note, NoteEvent, NoteFilter,
    Notes, Stats, TokenResponse,
};
use notes_server::{PAGE_SIZE, SESSION_LIFETIME, Sessions, Store, User};
use quoll::client;
use quoll::rest::{self, Form};
use reqwest::StatusCode;

fn served(app: notes_server::App) -> axum::Router {
    notes_server::server().into_router(notes_server::router(app))
}

#[tokio::test]
async fn a_created_note_reads_back() -> anyhow::Result<()> {
    let app = notes_server::App::default();
    let token = app.sessions.open(User {
        id: 1,
        name: "ana".into(),
    });
    let url = quoll::test::spawn(served(app)).await;
    let api = client::builder()
        .with(client::bearer(token))
        .build::<Api>(&url)?;

    let created = api
        .notes()
        .create(NewNote {
            title: "Milk".into(),
            body: String::new(),
        })
        .await?;
    let read = api.notes().get(created.id).await?;

    assert_eq!(read.title, "Milk");
    Ok(())
}

#[tokio::test]
async fn a_call_without_token_is_refused() -> anyhow::Result<()> {
    let url = quoll::test::spawn(served(notes_server::App::default())).await;
    let api = Api::client(&url)?;

    let error = api.notes().get(1).await.unwrap_err();

    assert_eq!(error.status(), Some(reqwest::StatusCode::UNAUTHORIZED));
    Ok(())
}

struct Served {
    url: String,
    store: Store,
    sessions: Sessions,
}

impl Served {
    async fn start() -> Self {
        let app = notes_server::App::default();
        let store = app.store.clone();
        let sessions = app.sessions.clone();
        let url = quoll::test::spawn(served(app)).await;

        Self {
            url,
            store,
            sessions,
        }
    }

    fn client_as(&self, user: User) -> rest::Result<ApiClient> {
        self.client_with(self.sessions.open(user))
    }

    fn client_with(&self, token: String) -> rest::Result<ApiClient> {
        client::builder()
            .with(client::bearer(token))
            .build::<Api>(&self.url)
    }

    fn anonymous(&self) -> rest::Result<ApiClient> {
        Api::client(&self.url)
    }
}

fn ana() -> User {
    User {
        id: 1,
        name: "ana".into(),
    }
}

fn bob() -> User {
    User {
        id: 2,
        name: "bob".into(),
    }
}

fn titled(title: &str) -> NewNote {
    NewNote {
        title: title.into(),
        body: String::new(),
    }
}

fn written(title: &str, body: &str) -> NewNote {
    NewNote {
        title: title.into(),
        body: body.into(),
    }
}

fn login(user: &str, password: &str) -> Form<Login> {
    Form(Login {
        user: user.into(),
        password: password.into(),
    })
}

fn titles(notes: &[Note]) -> Vec<&str> {
    notes.iter().map(|note| note.title.as_str()).collect()
}

#[tokio::test]
async fn a_created_note_has_its_body_and_a_new_id() -> anyhow::Result<()> {
    let api = Served::start().await.client_as(ana())?;

    let first = api.notes().create(written("Milk", "Two litres")).await?;
    let second = api.notes().create(titled("Bread")).await?;
    let read = api.notes().get(first.id).await?;

    assert_ne!(first.id, second.id);
    assert_eq!(
        (read.id, read.title.as_str(), read.body.as_str()),
        (first.id, "Milk", "Two litres")
    );
    Ok(())
}

#[tokio::test]
async fn a_login_gives_a_token_for_its_user() -> anyhow::Result<()> {
    let served = Served::start().await;
    let user = served.store.add_user("ana", "correct horse").await?;

    let token = served
        .anonymous()?
        .auth()
        .token(login("ana", "correct horse"))
        .await?;
    let api = served.client_with(token.access_token.clone())?;
    let created = api.notes().create(titled("Milk")).await?;

    assert_eq!(
        served.sessions.user(&token.access_token),
        Some(user.clone())
    );
    assert_eq!(
        served
            .store
            .note(user.id, created.id)
            .map(|note| note.title),
        Some("Milk".into())
    );
    Ok(())
}

#[tokio::test]
async fn a_wrong_password_and_an_unknown_user_are_refused() -> anyhow::Result<()> {
    let served = Served::start().await;

    served.store.add_user("ana", "correct horse").await?;

    for (user, password) in [("ana", "wrong"), ("ana", ""), ("bob", "correct horse")] {
        let error = served
            .anonymous()?
            .auth()
            .token(login(user, password))
            .await
            .unwrap_err();

        assert_eq!(error.status(), Some(StatusCode::UNAUTHORIZED), "{user}");
        assert_eq!(
            error.problem().and_then(|problem| problem.detail()),
            Some("wrong user or password")
        );
    }

    Ok(())
}

#[tokio::test]
async fn a_user_whose_name_is_taken_is_not_added() -> anyhow::Result<()> {
    let store = Store::default();

    store.add_user("ana", "correct horse").await?;

    assert!(store.add_user("ana", "another").await.is_err());
    assert!(store.check_password("ana", "another").await?.is_none());
    Ok(())
}

#[tokio::test]
async fn a_search_with_no_key_gives_every_note_of_its_user() -> anyhow::Result<()> {
    let served = Served::start().await;
    let ana_api = served.client_as(ana())?;
    let bob_api = served.client_as(bob())?;

    ana_api.notes().create(titled("Milk")).await?;
    bob_api.notes().create(titled("Beer")).await?;
    ana_api.notes().create(titled("Bread")).await?;

    let notes = ana_api.notes().search(NoteFilter::default()).await?;

    assert_eq!(titles(&notes), ["Milk", "Bread"]);
    Ok(())
}

#[tokio::test]
async fn a_search_by_contains_reads_the_title_and_the_body() -> anyhow::Result<()> {
    let api = Served::start().await.client_as(ana())?;

    api.notes().create(titled("Oat milk")).await?;
    api.notes()
        .create(written("Shopping", "MILK and eggs"))
        .await?;
    api.notes().create(titled("Bread")).await?;
    api.notes().create(titled("50% off")).await?;
    api.notes().create(titled("500 off")).await?;

    let milk = api
        .notes()
        .search(NoteFilter {
            contains: Some("milk".into()),
            ..Default::default()
        })
        .await?;
    let percent = api
        .notes()
        .search(NoteFilter {
            contains: Some("0%".into()),
            ..Default::default()
        })
        .await?;

    assert_eq!(titles(&milk), ["Oat milk", "Shopping"]);
    assert_eq!(titles(&percent), ["50% off"]);
    Ok(())
}

#[tokio::test]
async fn a_search_by_tag_reads_the_hash_words() -> anyhow::Result<()> {
    let api = Served::start().await.client_as(ana())?;

    api.notes()
        .create(written("Milk", "#Shopping, #dairy"))
        .await?;
    api.notes().create(titled("Bread #shopping")).await?;
    api.notes()
        .create(written("Shop", "#shop and #shoppingmall"))
        .await?;

    let tagged = |tag: &str| NoteFilter {
        tag: Some(tag.into()),
        ..Default::default()
    };

    assert_eq!(
        titles(&api.notes().search(tagged("shopping")).await?),
        ["Milk", "Bread #shopping"]
    );
    assert_eq!(
        titles(&api.notes().search(tagged("#DAIRY")).await?),
        ["Milk"]
    );
    assert!(api.notes().search(tagged("sho")).await?.is_empty());
    Ok(())
}

#[tokio::test]
async fn an_empty_key_of_a_search_is_no_key() -> anyhow::Result<()> {
    let api = Served::start().await.client_as(ana())?;

    api.notes().create(written("Milk", "#shopping")).await?;
    api.notes().create(titled("Bread")).await?;

    let notes = api
        .notes()
        .search(NoteFilter {
            contains: Some(String::new()),
            tag: Some(String::new()),
            page: None,
        })
        .await?;
    // A lone `#`, or a tag that cuts to nothing, is no tag either.
    let hash = api
        .notes()
        .search(NoteFilter {
            tag: Some("#".into()),
            ..Default::default()
        })
        .await?;

    assert_eq!(titles(&notes), ["Milk", "Bread"]);
    assert_eq!(titles(&hash), ["Milk", "Bread"]);
    Ok(())
}

#[tokio::test]
async fn the_tag_of_a_search_is_cut_as_the_tags_of_a_note() -> anyhow::Result<()> {
    let api = Served::start().await.client_as(ana())?;

    api.notes()
        .create(written("Milk", "#milk, two litres"))
        .await?;
    api.notes().create(titled("Bread")).await?;

    let notes = api
        .notes()
        .search(NoteFilter {
            tag: Some("#Milk,".into()),
            ..Default::default()
        })
        .await?;

    assert_eq!(titles(&notes), ["Milk"]);
    Ok(())
}

#[tokio::test]
async fn a_contains_past_its_limit_is_refused() -> anyhow::Result<()> {
    let api = Served::start().await.client_as(ana())?;
    let contains = |text: String| NoteFilter {
        contains: Some(text),
        ..Default::default()
    };

    // The limit counts characters, not bytes: 1000 of two bytes each are taken.
    let at_limit = api
        .notes()
        .search(contains("é".repeat(CONTAINS_LIMIT)))
        .await?;
    let past_limit = api
        .notes()
        .search(contains("a".repeat(CONTAINS_LIMIT + 1)))
        .await
        .unwrap_err();

    assert!(at_limit.is_empty());
    assert_eq!(past_limit.status(), Some(StatusCode::BAD_REQUEST));
    assert_eq!(
        past_limit.problem().and_then(|problem| problem.detail()),
        Some("`contains` holds more than 1000 characters")
    );
    Ok(())
}

#[tokio::test]
async fn a_search_by_tag_follows_an_update() -> anyhow::Result<()> {
    let api = Served::start().await.client_as(ana())?;
    let note = api.notes().create(written("Milk", "#shopping")).await?;

    api.notes()
        .update(note.id, written("Milk", "#done"))
        .await?;

    let shopping = api
        .notes()
        .search(NoteFilter {
            tag: Some("shopping".into()),
            ..Default::default()
        })
        .await?;
    let done = api
        .notes()
        .search(NoteFilter {
            tag: Some("done".into()),
            ..Default::default()
        })
        .await?;

    assert!(shopping.is_empty());
    assert_eq!(titles(&done), ["Milk"]);
    Ok(())
}

#[tokio::test]
async fn a_search_gives_a_page_at_a_time() -> anyhow::Result<()> {
    let api = Served::start().await.client_as(ana())?;

    for i in 1..=PAGE_SIZE + 1 {
        api.notes().create(titled(&format!("Note {i}"))).await?;
    }

    let page = |page: Option<u32>| NoteFilter {
        page,
        ..Default::default()
    };
    let first = api.notes().search(page(None)).await?;

    assert_eq!(first.len(), 20);
    assert_eq!(first[0].title, "Note 1");
    assert_eq!(api.notes().search(page(Some(1))).await?, first);
    assert_eq!(api.notes().search(page(Some(0))).await?, first);
    assert_eq!(
        titles(&api.notes().search(page(Some(2))).await?),
        ["Note 21"]
    );
    assert!(api.notes().search(page(Some(3))).await?.is_empty());
    Ok(())
}

#[tokio::test]
async fn a_search_combines_its_keys() -> anyhow::Result<()> {
    let api = Served::start().await.client_as(ana())?;

    api.notes().create(written("Oat milk", "#shopping")).await?;
    api.notes().create(written("Milk", "#done")).await?;
    api.notes().create(written("Bread", "#shopping")).await?;

    let notes = api
        .notes()
        .search(NoteFilter {
            contains: Some("milk".into()),
            tag: Some("shopping".into()),
            page: Some(1),
        })
        .await?;

    assert_eq!(titles(&notes), ["Oat milk"]);
    Ok(())
}

#[tokio::test]
async fn an_updated_note_reads_back_and_tells_its_watchers() -> anyhow::Result<()> {
    let api = Served::start().await.client_as(ana())?;
    let created = api.notes().create(titled("Milk")).await?;
    // The stream is there once its answer has come: the store watches the note by then.
    let mut events = api.notes().events(created.id).await?;

    let updated = api
        .notes()
        .update(created.id, written("Oat milk", "Two litres"))
        .await?;
    let read = api.notes().get(created.id).await?;
    let event = events.next().await.transpose()?;

    assert_eq!(updated, read);
    assert_eq!(
        read,
        Note {
            id: created.id,
            title: "Oat milk".into(),
            body: "Two litres".into(),
        }
    );
    assert_eq!(event, Some(NoteEvent::Edited(read)));
    Ok(())
}

#[tokio::test]
async fn a_removed_note_tells_its_watchers_and_ends_their_streams() -> anyhow::Result<()> {
    let api = Served::start().await.client_as(ana())?;
    let created = api.notes().create(titled("Milk")).await?;
    let mut first = api.notes().events(created.id).await?;
    let mut second = api.notes().events(created.id).await?;

    api.notes().update(created.id, titled("Oat milk")).await?;
    api.notes().remove(created.id).await?;

    for events in [&mut first, &mut second] {
        let Some(Ok(NoteEvent::Edited(edited))) = events.next().await else {
            panic!("the update is the first event");
        };

        assert_eq!(edited.title, "Oat milk");
        assert_eq!(
            events.next().await.transpose()?,
            Some(NoteEvent::Removed { id: created.id })
        );
        assert!(
            events.next().await.is_none(),
            "the stream ends after the removal"
        );
    }

    Ok(())
}

#[tokio::test]
async fn the_events_of_a_missing_note_are_not_found() -> anyhow::Result<()> {
    let api = Served::start().await.client_as(ana())?;
    let removed = api.notes().create(titled("Milk")).await?;

    api.notes().remove(removed.id).await?;

    for id in [999, removed.id] {
        let error = api.notes().events(id).await.unwrap_err();

        assert_eq!(error.status(), Some(StatusCode::NOT_FOUND));
        assert_eq!(
            error.problem().and_then(|problem| problem.detail()),
            Some("no such note")
        );
    }

    Ok(())
}

#[tokio::test]
async fn an_update_of_a_missing_note_is_not_found() -> anyhow::Result<()> {
    let api = Served::start().await.client_as(ana())?;

    let error = api.notes().update(999, titled("Milk")).await.unwrap_err();

    assert_eq!(error.status(), Some(StatusCode::NOT_FOUND));
    Ok(())
}

#[tokio::test]
async fn a_removed_note_is_not_found() -> anyhow::Result<()> {
    let api = Served::start().await.client_as(ana())?;
    let created = api.notes().create(titled("Milk")).await?;

    // A removal answers `()`, which the pattern checks.
    let () = api.notes().remove(created.id).await?;
    let error = api.notes().get(created.id).await.unwrap_err();

    assert_eq!(error.status(), Some(StatusCode::NOT_FOUND));
    assert_eq!(
        error.problem().and_then(|problem| problem.detail()),
        Some("no such note")
    );
    Ok(())
}

#[tokio::test]
async fn a_removal_asked_twice_succeeds_twice() -> anyhow::Result<()> {
    let api = Served::start().await.client_as(ana())?;
    let created = api.notes().create(titled("Milk")).await?;

    api.notes().remove(created.id).await?;
    api.notes().remove(created.id).await?;
    Ok(())
}

#[tokio::test]
async fn the_id_of_a_removed_note_is_not_given_again() -> anyhow::Result<()> {
    let api = Served::start().await.client_as(ana())?;
    let removed = api.notes().create(titled("Milk")).await?;

    api.notes().remove(removed.id).await?;

    let created = api.notes().create(titled("Bread")).await?;

    assert!(created.id > removed.id);
    Ok(())
}

#[tokio::test]
async fn a_note_of_another_user_is_neither_seen_nor_changed() -> anyhow::Result<()> {
    let served = Served::start().await;
    let ana_api = served.client_as(ana())?;
    let bob_api = served.client_as(bob())?;
    let created = ana_api.notes().create(written("Milk", "#shopping")).await?;
    let mut ana_events = ana_api.notes().events(created.id).await?;

    let read = bob_api.notes().get(created.id).await.unwrap_err();
    let updated = bob_api
        .notes()
        .update(created.id, titled("Beer"))
        .await
        .unwrap_err();
    let watched = bob_api.notes().events(created.id).await.unwrap_err();
    let found = bob_api
        .notes()
        .search(NoteFilter {
            tag: Some("shopping".into()),
            ..Default::default()
        })
        .await?;

    bob_api.notes().remove(created.id).await?;

    assert_eq!(read.status(), Some(StatusCode::NOT_FOUND));
    assert_eq!(updated.status(), Some(StatusCode::NOT_FOUND));
    assert_eq!(watched.status(), Some(StatusCode::NOT_FOUND));
    assert!(found.is_empty());
    assert_eq!(ana_api.notes().get(created.id).await?, created);

    // Neither bob's update nor his removal reached ana's watcher: her own update is the first event it reads.
    ana_api
        .notes()
        .update(created.id, titled("Oat milk"))
        .await?;

    let Some(Ok(NoteEvent::Edited(edited))) = ana_events.next().await else {
        panic!("ana's update is the first event");
    };

    assert_eq!(edited.title, "Oat milk");
    Ok(())
}

#[tokio::test]
async fn an_unknown_token_is_refused() -> anyhow::Result<()> {
    let served = Served::start().await;
    let token = served.sessions.open(ana());
    let api = served.client_with(format!("{token}0"))?;

    let error = api.notes().create(titled("Milk")).await.unwrap_err();

    assert_eq!(error.status(), Some(StatusCode::UNAUTHORIZED));
    Ok(())
}

#[tokio::test]
async fn an_operator_reads_the_stats_with_their_key() -> anyhow::Result<()> {
    let served = Served::start().await;
    let ana = served.store.add_user("ana", "correct horse").await?;
    let bob = served.store.add_user("bob", "battery staple").await?;
    let key = served.store.add_operator("ops");

    served
        .client_as(ana)?
        .notes()
        .create(titled("Milk"))
        .await?;
    served
        .client_as(bob.clone())?
        .notes()
        .create(titled("Beer"))
        .await?;
    served
        .client_as(bob)?
        .notes()
        .create(titled("Bread"))
        .await?;

    let operator = client::builder()
        .with(client::key::<Key>(key))
        .build::<Api>(&served.url)?;
    let stats = operator.admin().stats().await?;

    assert_eq!(stats, Stats { users: 2, notes: 3 });
    Ok(())
}

#[tokio::test]
async fn the_stats_are_refused_without_an_operators_key() -> anyhow::Result<()> {
    let served = Served::start().await;
    let key = served.store.add_operator("ops");
    let wrong_key = client::builder()
        .with(client::key::<Key>(format!("{key}0")))
        .build::<Api>(&served.url)?;
    // A user's token is no operator's key: the client does not even send it to a route of `Key`.
    let user = served.client_as(ana())?;

    for api in [served.anonymous()?, wrong_key, user] {
        let error = api.admin().stats().await.unwrap_err();

        assert_eq!(error.status(), Some(StatusCode::UNAUTHORIZED));
    }

    Ok(())
}

#[test]
fn a_login_and_a_token_show_no_secret() {
    let login = login("ana", "correct horse").0;
    let token = TokenResponse {
        access_token: "t0k3n".into(),
    };

    assert_eq!(format!("{login:?}"), r#"Login { user: "ana", .. }"#);
    assert_eq!(format!("{token:?}"), "TokenResponse { .. }");
}

#[test]
fn each_session_has_its_own_token() {
    let sessions = Sessions::default();

    let first = sessions.open(ana());
    let second = sessions.open(ana());

    assert_ne!(first, second);
    assert_eq!(first.len(), 64);
    assert!(first.bytes().all(|byte| byte.is_ascii_hexdigit()));
    assert_eq!(sessions.user(&first).map(|user| user.id), Some(1));
}

#[test]
fn a_session_past_its_lifetime_is_refused_and_dropped() {
    let sessions = Sessions::with_lifetime(Duration::ZERO);
    let token = sessions.open(ana());

    assert_eq!(sessions.user(&token), None);
    assert_eq!(
        format!("{sessions:?}"),
        "Sessions { lifetime: 0ns, open: 0 }"
    );
}

#[test]
fn opening_a_session_drops_those_past_their_lifetime() {
    let sessions = Sessions::with_lifetime(Duration::ZERO);

    sessions.open(ana());
    sessions.open(bob());
    sessions.open(ana());

    assert_eq!(
        format!("{sessions:?}"),
        "Sessions { lifetime: 0ns, open: 1 }"
    );
}

#[test]
fn a_session_lasts_a_day() {
    let sessions = Sessions::default();
    let token = sessions.open(ana());

    assert_eq!(SESSION_LIFETIME, Duration::from_secs(24 * 60 * 60));
    assert_eq!(sessions.user(&token), Some(ana()));
}

#[tokio::test]
async fn a_token_past_its_lifetime_is_refused() -> anyhow::Result<()> {
    let app = notes_server::App {
        store: Store::default(),
        sessions: Sessions::with_lifetime(Duration::ZERO),
    };
    let token = app.sessions.open(ana());
    let url = quoll::test::spawn(served(app)).await;
    let api = client::builder()
        .with(client::bearer(token))
        .build::<Api>(&url)?;

    let error = api.notes().get(1).await.unwrap_err();

    assert_eq!(error.status(), Some(StatusCode::UNAUTHORIZED));
    assert_eq!(
        error.problem().and_then(|problem| problem.detail()),
        Some("unknown or expired token")
    );
    Ok(())
}

#[test]
fn the_sessions_debug_shows_no_token() {
    let sessions = Sessions::default();
    let token = sessions.open(ana());

    let shown = format!("{sessions:?}");

    assert_eq!(shown, "Sessions { lifetime: 86400s, open: 1 }");
    assert!(!shown.contains(&token));
}

#[tokio::test]
async fn an_answer_carries_the_request_id_its_caller_gave() -> anyhow::Result<()> {
    let served = Served::start().await;

    let response = reqwest::Client::new()
        .get(format!("{}/api/notes/1", served.url))
        .header("x-request-id", "call-1")
        .send()
        .await?;

    assert_eq!(response.status(), StatusCode::UNAUTHORIZED);
    assert_eq!(response.headers()["x-request-id"], "call-1");
    Ok(())
}
