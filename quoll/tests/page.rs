//! The page a bundler builds, served beside the api by `server::builder().page(dir)`, and called over HTTP.

#![cfg(feature = "server")]

use std::fs;
use std::path::Path;

use quoll::{Response, Result, server};
use reqwest::StatusCode;
use reqwest::header::{
    ACCEPT, ACCEPT_ENCODING, CACHE_CONTROL, CONTENT_ENCODING, CONTENT_LENGTH, CONTENT_TYPE,
    IF_MODIFIED_SINCE, LAST_MODIFIED, LOCATION, RANGE, VARY,
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

/// A page as Vite builds it: its `index.html`, a script named by its hash, stored compressed beside itself too, a file
/// of `public/`, a hidden file, a well-known file and a directory of a multi-page build.
fn page() -> TempDir {
    let dir = tempfile::tempdir().expect("a temporary directory is made");

    write(dir.path(), "index.html", INDEX.as_bytes());
    write(dir.path(), HASHED, SCRIPT.as_bytes());
    write(dir.path(), &format!("{HASHED}.br"), b"brotli bytes");
    write(dir.path(), &format!("{HASHED}.gz"), b"gzip bytes");
    write(dir.path(), &format!("{HASHED}.zst"), b"zstd bytes");
    write(dir.path(), "robots.txt", b"User-agent: *");
    write(dir.path(), ".env", b"SECRET=1");
    write(dir.path(), ".well-known/security.txt", b"Contact: ops");
    write(dir.path(), "docs/index.html", b"<title>docs</title>");
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

/// A client that sends the `Accept-Encoding` it is told to, reads the bytes as they come, and follows no redirection.
fn client() -> reqwest::Client {
    reqwest::Client::builder()
        .no_gzip()
        .redirect(reqwest::redirect::Policy::none())
        .build()
        .expect("a client is built")
}

/// A request as a script or `fetch` sends it, which accepts `*/*`.
async fn get(url: &str) -> reqwest::Response {
    client().get(url).send().await.expect("the server answers")
}

/// The `Accept` of a browser's request of a page.
const BROWSER: &str = "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8";

/// A request as a browser sends it for a page.
async fn browse(url: &str) -> reqwest::Response {
    client()
        .get(url)
        .header(ACCEPT, BROWSER)
        .send()
        .await
        .expect("the server answers")
}

/// The values of every `Vary` of `response`, in lowercase.
fn varies(response: &reqwest::Response) -> Vec<String> {
    response
        .headers()
        .get_all(VARY)
        .iter()
        .flat_map(|value| value.to_str().expect("an ASCII header").split(','))
        .map(|value| value.trim().to_ascii_lowercase())
        .collect()
}

/// Checks that `response` is a problem 404 of `detail`.
async fn not_found(response: reqwest::Response, detail: &str) {
    assert_eq!(response.status(), StatusCode::NOT_FOUND);
    assert_eq!(header(&response, CONTENT_TYPE), "application/problem+json");

    let problem: Value = response.json().await.expect("a problem");

    assert_eq!(problem["detail"], detail);
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
    raw_with(url, target, "Accept: */*").await
}

/// The answer to a request for `target` written by hand, with one more header line.
async fn raw_with(url: &str, target: &str, line: &str) -> String {
    let address = url.strip_prefix("http://").expect("an http URL");
    let mut stream = TcpStream::connect(address)
        .await
        .expect("the server accepts");
    let request =
        format!("GET {target} HTTP/1.1\r\nHost: {address}\r\n{line}\r\nConnection: close\r\n\r\n");
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
async fn a_file_is_sent_with_the_content_type_of_its_extension_and_nosniff() {
    let dir = page();
    let url = serve(dir.path()).await;

    let response = get(&format!("{url}/robots.txt")).await;

    assert_eq!(response.status(), StatusCode::OK);
    assert_eq!(header(&response, CONTENT_TYPE), "text/plain");
    assert_eq!(header(&response, "x-content-type-options"), "nosniff");
    assert_eq!(header(&response, CONTENT_LENGTH), "13");
    assert!(!header(&response, LAST_MODIFIED).is_empty());
    assert!(response.headers().get("etag").is_none());
    assert_eq!(response.text().await.expect("a text"), "User-agent: *");
}

#[tokio::test]
async fn a_hashed_file_is_immutable_and_any_other_no_cache() {
    let dir = page();
    let url = serve(dir.path()).await;

    let script = get(&format!("{url}/{HASHED}")).await;
    let root = get(&format!("{url}/")).await;
    let robots = get(&format!("{url}/robots.txt")).await;

    assert_eq!(
        header(&script, CACHE_CONTROL),
        "public, max-age=31536000, immutable"
    );
    assert_eq!(header(&script, CONTENT_TYPE), "text/javascript");
    assert_eq!(header(&root, CACHE_CONTROL), "no-cache");
    assert_eq!(header(&root, CONTENT_TYPE), "text/html");
    assert_eq!(header(&robots, CACHE_CONTROL), "no-cache");
    assert_eq!(script.text().await.expect("a text"), SCRIPT);
    assert_eq!(root.text().await.expect("a text"), INDEX);
}

#[tokio::test]
async fn with_a_manifest_only_the_hashed_files_it_names_are_immutable() {
    let dir = page();

    write(
        dir.path(),
        "assets/main-abcdefgh.js",
        b"named by the manifest",
    );
    write(dir.path(), "assets/main-abcdefgh.js.map", b"{}");
    write(
        dir.path(),
        ".vite/manifest.json",
        br#"{"index.html": {"file": "assets/main-abcdefgh.js", "isEntry": true}}"#,
    );

    let url = serve(dir.path()).await;
    let cache = |path: &'static str| {
        let url = url.clone();

        async move { header(&get(&format!("{url}/{path}")).await, CACHE_CONTROL).to_owned() }
    };

    assert_eq!(
        cache("assets/main-abcdefgh.js").await,
        "public, max-age=31536000, immutable"
    );
    assert_eq!(
        cache("assets/main-abcdefgh.js.map").await,
        "public, max-age=31536000, immutable"
    );
    // Named as a hash, but not by the manifest.
    assert_eq!(cache(HASHED).await, "no-cache");
    not_found(
        get(&format!("{url}/.vite/manifest.json")).await,
        "the page serves no hidden file, whose name begins with a `.`",
    )
    .await;

    // A new deployment writes a new manifest, read again.
    write(
        dir.path(),
        ".vite/manifest.json",
        format!(r#"{{"index.html": {{"file": "{HASHED}", "isEntry": true}}}}"#).as_bytes(),
    );

    assert_eq!(cache(HASHED).await, "public, max-age=31536000, immutable");
    assert_eq!(cache("assets/main-abcdefgh.js").await, "no-cache");
}

#[tokio::test]
async fn a_request_of_a_page_that_is_no_file_answers_index_html() {
    let dir = page();
    let url = serve(dir.path()).await;

    for path in ["/notes/3", "/settings", "/notes/3/", "/users/jane.doe"] {
        let response = browse(&format!("{url}{path}")).await;

        assert_eq!(response.status(), StatusCode::OK, "{path}");
        assert_eq!(header(&response, CACHE_CONTROL), "no-cache");
        assert_eq!(header(&response, CONTENT_TYPE), "text/html");
        assert_eq!(header(&response, "x-content-type-options"), "nosniff");
        assert!(varies(&response).contains(&"accept".to_owned()), "{path}");
        assert_eq!(response.text().await.expect("a text"), INDEX);
    }
}

#[tokio::test]
async fn a_request_that_accepts_no_html_is_a_problem_404_when_no_file() {
    let dir = page();
    let url = serve(dir.path()).await;

    for (path, accept) in [
        ("/assets/index-Gone1234.js", "*/*"),
        ("/assets/index-Gone1234.css", "text/css,*/*;q=0.1"),
        ("/favicon.ico", "image/avif,image/webp,*/*"),
        ("/notes/3", "application/json"),
        ("/notes/3", "text/html;q=0"),
    ] {
        let response = client()
            .get(format!("{url}{path}"))
            .header(ACCEPT, accept)
            .send()
            .await
            .expect("the server answers");

        assert_eq!(varies(&response), ["accept"], "{path} {accept}");
        not_found(response, "the page has no file of this path").await;
    }
}

#[tokio::test]
async fn a_missing_file_of_the_well_known_directory_is_never_index_html() {
    let dir = page();
    let url = serve(dir.path()).await;

    let found = browse(&format!("{url}/.well-known/security.txt")).await;

    assert_eq!(found.text().await.expect("a text"), "Contact: ops");

    // Told by the decoded segments: `/%2Ewell-known/` and `/./.well-known/` are `/.well-known/` too.
    for target in [
        "/.well-known/change-password",
        "/%2Ewell-known/change-password",
        "/./.well-known/change-password",
    ] {
        let answer = raw_with(&url, target, &format!("Accept: {BROWSER}")).await;

        assert!(answer.starts_with("HTTP/1.1 404"), "{target}: {answer}");
        assert!(!answer.contains(INDEX), "{target}: {answer}");
    }
}

#[tokio::test]
async fn index_html_is_always_sent_whole() {
    let dir = page();
    let url = serve(dir.path()).await;

    let modified = header(&get(&format!("{url}/")).await, LAST_MODIFIED).to_owned();

    #[cfg(unix)]
    std::os::unix::fs::symlink(dir.path().join("index.html"), dir.path().join("start.html"))
        .expect("the link is made");

    // `start.html` is `index.html` by its real path.
    let paths: &[&str] = if cfg!(unix) {
        &["/", "/index.html", "/docs/", "/notes/3", "/start.html"]
    } else {
        &["/", "/index.html", "/docs/", "/notes/3"]
    };

    for path in paths {
        let response = client()
            .get(format!("{url}{path}"))
            .header(ACCEPT, BROWSER)
            .header(IF_MODIFIED_SINCE, &modified)
            .header(RANGE, "bytes=0-3")
            .send()
            .await
            .expect("the server answers");

        assert_eq!(response.status(), StatusCode::OK, "{path}");
        assert!(response.text().await.expect("a text").len() > 4, "{path}");
    }
}

#[tokio::test]
async fn another_file_is_not_modified_since_its_date() {
    let dir = page();
    let url = serve(dir.path()).await;
    let robots = format!("{url}/robots.txt");

    let modified = header(&get(&robots).await, LAST_MODIFIED).to_owned();
    let again = client()
        .get(&robots)
        .header(IF_MODIFIED_SINCE, &modified)
        .send()
        .await
        .expect("the server answers");

    assert_eq!(again.status(), StatusCode::NOT_MODIFIED);
    assert_eq!(header(&again, CACHE_CONTROL), "no-cache");
}

#[tokio::test]
async fn a_directory_answers_its_index_html_and_without_its_slash_a_307() {
    let dir = page();
    let url = serve(dir.path()).await;

    let slashed = browse(&format!("{url}/docs/")).await;
    let bare = browse(&format!("{url}/docs?tab=a%20b")).await;

    assert_eq!(slashed.status(), StatusCode::OK);
    assert_eq!(header(&slashed, CACHE_CONTROL), "no-cache");
    assert_eq!(slashed.text().await.expect("a text"), "<title>docs</title>");
    assert_eq!(bare.status(), StatusCode::TEMPORARY_REDIRECT);
    assert_eq!(header(&bare, LOCATION), "/docs/?tab=a%20b");
}

#[tokio::test]
async fn the_api_still_answers_and_its_prefix_keeps_its_problem_404() {
    let dir = page();
    let url = serve(dir.path()).await;

    let note = get(&format!("{url}/api/notes/3")).await;

    assert_eq!(note.status(), StatusCode::OK);
    assert_eq!(note.json::<String>().await.expect("a string"), "note 3");

    for path in ["/api", "/api/", "/api/nope", "/api/notes/3/more"] {
        not_found(
            browse(&format!("{url}{path}")).await,
            "no route of the api serves this path",
        )
        .await;
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
    let other = browse(&format!("{url}/notes/3")).await;

    assert_eq!(health.json::<String>().await.expect("a string"), "up");
    assert_eq!(other.text().await.expect("a text"), INDEX);
}

#[tokio::test]
async fn a_file_stored_compressed_is_sent_when_its_coding_is_accepted() {
    let dir = page();
    let url = serve(dir.path()).await;
    let script = format!("{url}/{HASHED}");

    for (accepted, encoding, body) in [
        ("br", "br", "brotli bytes"),
        ("gzip", "gzip", "gzip bytes"),
        ("zstd", "zstd", "zstd bytes"),
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
        assert_eq!(header(&response, CONTENT_TYPE), "text/javascript");

        // Sent as it is or as a copy, the file has copies: its answer varies by the coding.
        assert_eq!(varies(&response), ["accept-encoding"], "{accepted}");

        assert_eq!(response.text().await.expect("a body"), body);
    }

    // A file with no copy does not vary.
    assert!(varies(&get(&format!("{url}/robots.txt")).await).is_empty());
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
    assert_eq!(header(&response, CONTENT_TYPE), "text/plain");
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
    assert_eq!(header(&response, "x-content-type-options"), "nosniff");
}

#[tokio::test]
async fn a_hidden_file_is_refused_but_the_well_known_directory() {
    let dir = page();
    let url = serve(dir.path()).await;

    let hidden = browse(&format!("{url}/.env")).await;
    let well_known = get(&format!("{url}/.well-known/security.txt")).await;

    assert_eq!(well_known.status(), StatusCode::OK);
    assert_eq!(well_known.text().await.expect("a text"), "Contact: ops");
    not_found(
        hidden,
        "the page serves no hidden file, whose name begins with a `.`",
    )
    .await;
}

#[tokio::test]
async fn a_path_not_utf8_says_so() {
    let dir = page();
    let url = serve(dir.path()).await;

    not_found(
        get(&format!("{url}/%FF.js")).await,
        "the path is not UTF-8 once decoded, which no file of the page is named in",
    )
    .await;
}

#[tokio::test]
async fn a_path_of_two_slashes_is_refused_before_it_is_redirected_to_another_host() {
    let dir = page();

    write(dir.path(), "evil.example/index.html", b"a directory");

    let url = serve(dir.path()).await;
    let answer = raw(&url, "//evil.example").await;

    assert!(answer.starts_with("HTTP/1.1 404"), "{answer}");
    assert!(
        !answer.to_ascii_lowercase().contains("location"),
        "{answer}"
    );
}

#[tokio::test]
async fn a_file_modified_before_1970_is_a_problem_500_not_a_panic() {
    let dir = page();
    let before = std::time::UNIX_EPOCH - std::time::Duration::from_secs(86_400);

    fs::File::options()
        .write(true)
        .open(dir.path().join("robots.txt"))
        .and_then(|file| file.set_modified(before))
        .expect("the time is set");

    let url = serve(dir.path()).await;

    // `ServeDir` opens a file whatever its final `/`.
    for path in ["/robots.txt", "/robots.txt/"] {
        let undated = get(&format!("{url}{path}")).await;

        assert_eq!(
            undated.status(),
            StatusCode::INTERNAL_SERVER_ERROR,
            "{path}"
        );
        assert_eq!(header(&undated, CONTENT_TYPE), "application/problem+json");
        assert_eq!(header(&undated, "x-content-type-options"), "nosniff");
    }

    assert_eq!(
        get(&format!("{url}/{HASHED}")).await.status(),
        StatusCode::OK
    );
}

#[cfg(unix)]
#[tokio::test]
async fn a_file_there_but_unreadable_is_a_problem_500_that_names_it() {
    use std::os::unix::fs::PermissionsExt;

    let dir = page();
    let locked = dir.path().join("locked.js");

    write(dir.path(), "locked.js", b"locked");
    fs::set_permissions(&locked, fs::Permissions::from_mode(0o000)).expect("the mode is set");

    // Run as root, the file reads all the same: there is nothing to test.
    if fs::read(&locked).is_ok() {
        return;
    }

    let url = serve(dir.path()).await;
    let response = browse(&format!("{url}/locked.js")).await;

    assert_eq!(response.status(), StatusCode::INTERNAL_SERVER_ERROR);
    assert_eq!(header(&response, CONTENT_TYPE), "application/problem+json");
    assert!(!response.text().await.expect("a body").contains(INDEX));

    fs::set_permissions(&locked, fs::Permissions::from_mode(0o644)).expect("the mode is set");
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
    let link = |target: &Path, name: &str| {
        std::os::unix::fs::symlink(target, dir.join(name)).expect("the link is made");
    };

    write(&dir, "index.html", INDEX.as_bytes());
    write(&dir, "inside/real.txt", b"inside");
    write(&dir, "within.txt", b"within");
    write(outer.path(), "secret.txt", b"the secret");
    link(&outer.path().join("secret.txt"), "linked.txt");
    link(outer.path(), "up");
    link(&dir.join("inside/real.txt"), "linked-within.txt");
    // A compressed copy that `ServeDir` would send in the file's place.
    link(&outer.path().join("secret.txt"), "within.txt.gz");

    let url = serve(&dir).await;

    for path in [
        "/linked.txt",
        "/linked.txt/",
        "/up/secret.txt",
        "/within.txt",
        "/within.txt/",
    ] {
        let response = client()
            .get(format!("{url}{path}"))
            .header(ACCEPT_ENCODING, "gzip")
            .send()
            .await
            .expect("the server answers");

        assert_eq!(response.status(), StatusCode::NOT_FOUND, "{path}");
        assert!(
            !response
                .text()
                .await
                .expect("a body")
                .contains("the secret")
        );
    }

    let within = get(&format!("{url}/linked-within.txt")).await;

    assert_eq!(within.status(), StatusCode::OK);
    assert_eq!(within.text().await.expect("a text"), "inside");
}

#[cfg(unix)]
#[tokio::test]
async fn a_symbolic_link_to_a_hidden_file_is_refused() {
    let dir = page();

    write(dir.path(), ".git/config", b"the secret");
    std::os::unix::fs::symlink(
        dir.path().join(".git/config"),
        dir.path().join("config.txt"),
    )
    .expect("the link is made");
    std::os::unix::fs::symlink(dir.path().join(".git"), dir.path().join("git"))
        .expect("the link is made");

    let url = serve(dir.path()).await;

    for path in ["/config.txt", "/config.txt/", "/git/config"] {
        let response = browse(&format!("{url}{path}")).await;

        assert_eq!(response.status(), StatusCode::NOT_FOUND, "{path}");
        assert!(
            !response
                .text()
                .await
                .expect("a body")
                .contains("the secret")
        );
    }
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
