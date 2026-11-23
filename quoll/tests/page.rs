//! The page a bundler builds, served beside the api by `server::builder().page(dir)`, and called over HTTP.

#![cfg(feature = "server")]

use std::fs;
use std::path::Path;

use quoll::{Response, Result, server};
use reqwest::StatusCode;
use reqwest::header::{
    ACCEPT_ENCODING, CACHE_CONTROL, CONTENT_ENCODING, CONTENT_LENGTH, CONTENT_TYPE, ETAG,
    IF_MODIFIED_SINCE, IF_NONE_MATCH, LAST_MODIFIED, VARY,
};
use serde_json::Value;
use tempfile::TempDir;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;

quoll::api! {
    /// An api beside a page.
    api Api = "/api";

    group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

quoll::api! {
    /// An api with no prefix, beside a page.
    api Bare = "";

    group Health {
        GET "/health" => health() -> String;
    }
}

struct App;

impl NotesServer for App {
    async fn get(&self, id: i64) -> Result<Response<String>> {
        Ok(Response::new(format!("note {id}")))
    }
}

impl HealthServer for App {
    async fn health(&self) -> Result<Response<String>> {
        Ok(Response::new("up".to_owned()))
    }
}

const INDEX: &str = "<!doctype html><title>notes</title>";
const SCRIPT: &str = "console.log('notes')";
const HASHED: &str = "assets/index-BXa3kD9f.js";

/// A page as Vite builds it: its `index.html`, a script named by its hash, stored compressed beside itself too, and
/// a file of `public/`.
fn page() -> TempDir {
    let dir = tempfile::tempdir().expect("a temporary directory is made");

    write(dir.path(), "index.html", INDEX.as_bytes());
    write(dir.path(), HASHED, SCRIPT.as_bytes());
    write(dir.path(), &format!("{HASHED}.br"), b"brotli bytes");
    write(dir.path(), &format!("{HASHED}.gz"), b"gzip bytes");
    write(dir.path(), "robots.txt", b"User-agent: *");
    dir
}

fn write(dir: &Path, path: &str, content: &[u8]) {
    let path = dir.join(path);

    fs::create_dir_all(path.parent().expect("a file has a parent")).expect("the directory is made");
    fs::write(path, content).expect("the file is written");
}

/// Serves `Api` with the page of `dir`, and gives the server's URL.
async fn serve(dir: &Path) -> String {
    let router = server::builder()
        .page(dir)
        .into_router(Api::router(ApiServices { notes: App }));

    quoll::test::spawn(router).await
}

/// A client that sends the `Accept-Encoding` it is told to, and reads the bytes as they come.
fn client() -> reqwest::Client {
    reqwest::Client::builder()
        .no_gzip()
        .build()
        .expect("a client is built")
}

async fn get(url: &str) -> reqwest::Response {
    client().get(url).send().await.expect("the server answers")
}

fn header(response: &reqwest::Response, name: impl reqwest::header::AsHeaderName) -> &str {
    response
        .headers()
        .get(name)
        .map(|value| value.to_str().expect("an ASCII header"))
        .unwrap_or_default()
}

/// The status line and the body of a request for `target` written by hand, as a client that does not resolve `..`
/// sends it.
async fn raw(url: &str, target: &str) -> String {
    let address = url.strip_prefix("http://").expect("an http URL");
    let mut stream = TcpStream::connect(address)
        .await
        .expect("the server accepts");
    let request = format!("GET {target} HTTP/1.1\r\nHost: {address}\r\nConnection: close\r\n\r\n");
    let mut answer = String::new();

    stream
        .write_all(request.as_bytes())
        .await
        .expect("the request is sent");
    stream
        .read_to_string(&mut answer)
        .await
        .expect("the answer is read");
    answer
}

#[tokio::test]
async fn a_file_is_sent_with_the_content_type_of_its_extension() {
    let dir = page();
    let url = serve(dir.path()).await;

    let response = get(&format!("{url}/robots.txt")).await;

    assert_eq!(response.status(), StatusCode::OK);
    assert_eq!(header(&response, CONTENT_TYPE), "text/plain; charset=utf-8");
    assert_eq!(header(&response, "x-content-type-options"), "nosniff");
    assert_eq!(header(&response, CONTENT_LENGTH), "13");
    assert_eq!(response.text().await.expect("a text"), "User-agent: *");
}

#[tokio::test]
async fn a_hashed_file_is_immutable_and_index_html_is_checked_again() {
    let dir = page();
    let url = serve(dir.path()).await;

    let script = get(&format!("{url}/{HASHED}")).await;
    let root = get(&format!("{url}/")).await;
    let robots = get(&format!("{url}/robots.txt")).await;

    assert_eq!(
        header(&script, CACHE_CONTROL),
        "public, max-age=31536000, immutable"
    );
    assert_eq!(
        header(&script, CONTENT_TYPE),
        "text/javascript; charset=utf-8"
    );
    assert_eq!(header(&root, CACHE_CONTROL), "no-cache");
    assert_eq!(header(&root, CONTENT_TYPE), "text/html; charset=utf-8");
    assert_eq!(header(&robots, CACHE_CONTROL), "no-cache");
    assert_eq!(script.text().await.expect("a text"), SCRIPT);
    assert_eq!(root.text().await.expect("a text"), INDEX);
}

#[tokio::test]
async fn a_path_that_is_no_file_and_no_route_answers_index_html() {
    let dir = page();
    let url = serve(dir.path()).await;

    for path in ["/notes/3", "/settings", "/assets"] {
        let response = get(&format!("{url}{path}")).await;

        assert_eq!(response.status(), StatusCode::OK, "{path}");
        assert_eq!(header(&response, CACHE_CONTROL), "no-cache");
        assert_eq!(header(&response, CONTENT_TYPE), "text/html; charset=utf-8");
        assert_eq!(response.text().await.expect("a text"), INDEX);
    }
}

#[tokio::test]
async fn the_api_still_answers_and_its_prefix_keeps_its_problem_404() {
    let dir = page();
    let url = serve(dir.path()).await;

    let note = get(&format!("{url}/api/notes/3")).await;

    assert_eq!(note.status(), StatusCode::OK);
    assert_eq!(note.json::<String>().await.expect("a string"), "note 3");

    for path in ["/api", "/api/", "/api/nope", "/api/notes/3/more"] {
        let response = get(&format!("{url}{path}")).await;

        assert_eq!(response.status(), StatusCode::NOT_FOUND, "{path}");
        assert_eq!(header(&response, CONTENT_TYPE), "application/problem+json");

        let problem: Value = response.json().await.expect("a problem");

        assert_eq!(problem["detail"], "no route of the api serves this path");
    }

    let wrong_method = client()
        .post(format!("{url}/api/notes/3"))
        .send()
        .await
        .expect("the server answers");

    assert_eq!(wrong_method.status(), StatusCode::METHOD_NOT_ALLOWED);
}

#[tokio::test]
async fn an_api_with_no_prefix_leaves_every_other_path_to_the_page() {
    let dir = page();
    let router = server::builder()
        .page(dir.path())
        .into_router(Bare::router(BareServices { health: App }));
    let url = quoll::test::spawn(router).await;

    let health = get(&format!("{url}/health")).await;
    let other = get(&format!("{url}/notes/3")).await;

    assert_eq!(health.json::<String>().await.expect("a string"), "up");
    assert_eq!(other.text().await.expect("a text"), INDEX);
}

#[tokio::test]
async fn a_file_stored_compressed_is_sent_when_its_coding_is_accepted() {
    let dir = page();
    let url = serve(dir.path()).await;
    let script = format!("{url}/{HASHED}");

    for (accepted, encoding, body) in [
        ("gzip, deflate, br", "br", "brotli bytes"),
        ("gzip", "gzip", "gzip bytes"),
        ("br;q=0.5, gzip", "gzip", "gzip bytes"),
        ("identity", "", SCRIPT),
    ] {
        let response = client()
            .get(&script)
            .header(ACCEPT_ENCODING, accepted)
            .send()
            .await
            .expect("the server answers");

        assert_eq!(header(&response, CONTENT_ENCODING), encoding, "{accepted}");
        assert_eq!(header(&response, VARY), "accept-encoding");
        assert_eq!(
            header(&response, CONTENT_TYPE),
            "text/javascript; charset=utf-8"
        );
        assert_eq!(response.text().await.expect("a body"), body);
    }

    // `index.html` has no compressed copy: it is sent as it is, and still varies.
    let root = client()
        .get(format!("{url}/"))
        .header(ACCEPT_ENCODING, "br")
        .send()
        .await
        .expect("the server answers");

    assert_eq!(header(&root, CONTENT_ENCODING), "");
    assert_eq!(header(&root, VARY), "accept-encoding");
}

#[tokio::test]
async fn head_gets_the_headers_alone() {
    let dir = page();
    let url = serve(dir.path()).await;

    let response = client()
        .head(format!("{url}/robots.txt"))
        .send()
        .await
        .expect("the server answers");

    assert_eq!(response.status(), StatusCode::OK);
    assert_eq!(header(&response, CONTENT_LENGTH), "13");
    assert_eq!(header(&response, CONTENT_TYPE), "text/plain; charset=utf-8");
    assert!(!header(&response, ETAG).is_empty());
    assert!(response.bytes().await.expect("a body").is_empty());
}

#[tokio::test]
async fn another_method_is_a_problem_405() {
    let dir = page();
    let url = serve(dir.path()).await;

    let response = client()
        .post(format!("{url}/robots.txt"))
        .send()
        .await
        .expect("the server answers");

    assert_eq!(response.status(), StatusCode::METHOD_NOT_ALLOWED);
    assert_eq!(header(&response, "allow"), "GET, HEAD");
    assert_eq!(header(&response, CONTENT_TYPE), "application/problem+json");
}

#[tokio::test]
async fn a_copy_of_the_same_tag_or_date_is_not_modified() {
    let dir = page();
    let url = serve(dir.path()).await;
    let robots = format!("{url}/robots.txt");

    let first = get(&robots).await;
    let tag = header(&first, ETAG).to_owned();
    let modified = header(&first, LAST_MODIFIED).to_owned();

    assert!(tag.starts_with("W/\""), "{tag}");

    let by_tag = client()
        .get(&robots)
        .header(IF_NONE_MATCH, format!("\"other\", {tag}"))
        .send()
        .await
        .expect("the server answers");
    let by_date = client()
        .get(&robots)
        .header(IF_MODIFIED_SINCE, &modified)
        .send()
        .await
        .expect("the server answers");
    // A tag that is not the file's wins over a date that is.
    let other_tag = client()
        .get(&robots)
        .header(IF_NONE_MATCH, "\"other\"")
        .header(IF_MODIFIED_SINCE, &modified)
        .send()
        .await
        .expect("the server answers");

    assert_eq!(by_tag.status(), StatusCode::NOT_MODIFIED);
    assert_eq!(header(&by_tag, ETAG), tag);
    assert_eq!(header(&by_tag, CACHE_CONTROL), "no-cache");
    assert!(by_tag.bytes().await.expect("a body").is_empty());
    assert_eq!(by_date.status(), StatusCode::NOT_MODIFIED);
    assert_eq!(other_tag.status(), StatusCode::OK);
    assert_eq!(other_tag.text().await.expect("a text"), "User-agent: *");
}

#[tokio::test]
async fn a_coding_has_a_tag_of_its_own() {
    let dir = page();
    let url = serve(dir.path()).await;
    let script = format!("{url}/{HASHED}");
    let tag = |accepted: &'static str| {
        let script = script.clone();

        async move {
            let response = client()
                .get(script)
                .header(ACCEPT_ENCODING, accepted)
                .send()
                .await
                .expect("the server answers");

            header(&response, ETAG).to_owned()
        }
    };

    let brotli = tag("br").await;
    let gzip = tag("gzip").await;
    let plain = tag("identity").await;

    assert_ne!(brotli, gzip);
    assert_ne!(gzip, plain);
    assert_ne!(brotli, plain);
}

#[tokio::test]
async fn a_path_with_dot_dot_is_refused() {
    let outer = tempfile::tempdir().expect("a temporary directory is made");
    let dir = outer.path().join("dist");

    write(&dir, "index.html", INDEX.as_bytes());
    write(outer.path(), "secret.txt", b"the secret");

    let url = serve(&dir).await;

    for target in [
        "/../secret.txt",
        "/assets/../../secret.txt",
        "/%2e%2e/secret.txt",
        "/..%2Fsecret.txt",
    ] {
        let answer = raw(&url, target).await;

        assert!(answer.starts_with("HTTP/1.1 404"), "{target}: {answer}");
        assert!(!answer.contains("the secret"), "{target}: {answer}");
    }
}

#[cfg(unix)]
#[tokio::test]
async fn a_symbolic_link_out_of_the_directory_is_refused() {
    let outer = tempfile::tempdir().expect("a temporary directory is made");
    let dir = outer.path().join("dist");

    write(&dir, "index.html", INDEX.as_bytes());
    write(outer.path(), "secret.txt", b"the secret");
    write(&dir, "inside/real.txt", b"inside");
    std::os::unix::fs::symlink(outer.path().join("secret.txt"), dir.join("linked.txt"))
        .expect("the link is made");
    std::os::unix::fs::symlink(outer.path(), dir.join("up")).expect("the link is made");
    std::os::unix::fs::symlink(dir.join("inside/real.txt"), dir.join("within.txt"))
        .expect("the link is made");
    // A compressed copy out of the directory is passed over, the file sent as it is.
    std::os::unix::fs::symlink(outer.path().join("secret.txt"), dir.join("within.txt.gz"))
        .expect("the link is made");

    let url = serve(&dir).await;

    for path in ["/linked.txt", "/up/secret.txt"] {
        let response = get(&format!("{url}{path}")).await;

        assert_eq!(response.status(), StatusCode::NOT_FOUND, "{path}");
        assert!(
            !response
                .text()
                .await
                .expect("a body")
                .contains("the secret")
        );
    }

    let within = client()
        .get(format!("{url}/within.txt"))
        .header(ACCEPT_ENCODING, "gzip")
        .send()
        .await
        .expect("the server answers");

    assert_eq!(within.status(), StatusCode::OK);
    assert_eq!(header(&within, CONTENT_ENCODING), "");
    assert_eq!(within.text().await.expect("a text"), "inside");
}

#[tokio::test]
async fn a_server_refuses_to_start_with_no_index_html() {
    let dir = tempfile::tempdir().expect("a temporary directory is made");
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0")
        .await
        .expect("a port of 127.0.0.1 is free");

    let error = server::builder()
        .page(dir.path().join("dist"))
        .shutdown(std::future::pending())
        .serve_on(listener, Api::router(ApiServices { notes: App }))
        .await
        .expect_err("a page with no index.html is refused");

    assert_eq!(error.kind(), std::io::ErrorKind::NotFound);
    assert!(error.to_string().contains("index.html"), "{error}");
}
