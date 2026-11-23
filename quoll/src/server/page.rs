//! The page a bundler builds, served from its directory beside the api by tower-http's `ServeDir`, with thin layers
//! around it: a filter before it, the `Cache-Control` and the headers after it, and its fallback, `index.html` for a
//! browser's request of a page that is no file.

use std::collections::HashSet;
use std::convert::Infallible;
use std::error::Error as StdError;
use std::fmt;
use std::fs::Metadata;
use std::future::{Future, poll_fn};
use std::io;
use std::path::{Component, Path, PathBuf};
use std::pin::Pin;
use std::sync::{Arc, Mutex, PoisonError};
use std::task::{Context, Poll};
use std::time::{SystemTime, UNIX_EPOCH};

use axum::body::Body;
use axum::extract::Request;
use axum::response::{IntoResponse, Response};
use http::{HeaderMap, HeaderValue, Method, StatusCode, header};
use percent_encoding::percent_decode_str;
use tower_http::services::{ServeDir, ServeFile};
use tower_service::Service;

use crate::Error;

/// The `Cache-Control` of a file the bundler names by its hash: its content never changes under its name.
const IMMUTABLE: HeaderValue = HeaderValue::from_static("public, max-age=31536000, immutable");

/// The `Cache-Control` of any other file: kept, and checked again before each use.
const NO_CACHE: HeaderValue = HeaderValue::from_static("no-cache");

/// The file answered at `/`, in a directory, and for a browser's request of a page that is no file.
const INDEX: &str = "index.html";

/// The directory of the files the bundler names by their hash, as Vite names it.
const ASSETS: &str = "assets";

/// The manifest Vite writes with `build.manifest`, which names the files it hashed, under the page's directory.
const MANIFEST: &str = ".vite/manifest.json";

/// The well-known directory of RFC 8615, the one hidden directory a page serves, at its root.
const WELL_KNOWN: &str = ".well-known";

/// The extensions of the compressed copies `ServeDir` sends in the place of a file.
const COPIES: [&str; 3] = ["br", "gz", "zst"];

/// The files of a page's directory, a service an axum router falls back to.
#[derive(Debug, Clone)]
pub(crate) struct Page {
    dir: Arc<Path>,
    files: ServeDir<Fallback>,
    manifest: Arc<Manifest>,
}

impl Page {
    /// The page of the files in `dir`, read at each request.
    pub(crate) fn new(dir: PathBuf) -> Self {
        let dir: Arc<Path> = dir.into();
        let files = ServeDir::new(&*dir)
            .precompressed_br()
            .precompressed_gzip()
            .precompressed_zstd()
            .fallback(Fallback {
                dir: Arc::clone(&dir),
            });

        Self {
            dir,
            files,
            manifest: Arc::default(),
        }
    }
}

impl Service<Request> for Page {
    type Response = Response;
    type Error = Infallible;
    type Future = Pin<Box<dyn Future<Output = Result<Response, Infallible>> + Send>>;

    fn poll_ready(&mut self, _cx: &mut Context<'_>) -> Poll<Result<(), Infallible>> {
        Poll::Ready(Ok(()))
    }

    fn call(&mut self, request: Request) -> Self::Future {
        let page = self.clone();

        Box::pin(async move {
            let mut response = page.answer(request).await;
            let headers = response.headers_mut();

            // Every answer of the page, its errors among them, leaves through here. Any path may have compressed
            // copies, which `ServeDir` sends: every answer varies by the coding.
            headers.insert(
                header::X_CONTENT_TYPE_OPTIONS,
                HeaderValue::from_static("nosniff"),
            );
            vary(headers, "accept-encoding");
            Ok(response)
        })
    }
}

/// Adds `name` to the `Vary` of `headers`, once: the names they hold already are kept, each once, in one header.
fn vary(headers: &mut HeaderMap, name: &str) {
    let mut names: Vec<String> = Vec::new();
    let held = headers
        .get_all(header::VARY)
        .iter()
        .filter_map(|value| value.to_str().ok())
        .flat_map(|value| value.split(','))
        .map(|name| name.trim().to_ascii_lowercase());

    for held in held.chain(std::iter::once(name.to_owned())) {
        if !held.is_empty() && !names.contains(&held) {
            names.push(held);
        }
    }

    if let Ok(value) = HeaderValue::from_str(&names.join(", ")) {
        headers.insert(header::VARY, value);
    }
}

/// Checks that `dir` holds an `index.html`, before a server serves it.
///
/// # Errors
///
/// When `dir/index.html` cannot be read, or is not a file.
pub(crate) async fn check(dir: &Path) -> io::Result<()> {
    let index = dir.join(INDEX);

    match tokio::fs::metadata(&index).await {
        Ok(metadata) if metadata.is_file() => Ok(()),
        Ok(_) => Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            NoIndex {
                index,
                source: None,
            },
        )),
        Err(error) => Err(io::Error::new(
            error.kind(),
            NoIndex {
                index,
                source: Some(error),
            },
        )),
    }
}

/// The `index.html` of a page that cannot be served.
#[derive(Debug)]
struct NoIndex {
    index: PathBuf,
    source: Option<io::Error>,
}

impl fmt::Display for NoIndex {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.source {
            Some(_) => write!(f, "the page's `{}` cannot be read", self.index.display()),
            None => write!(f, "the page's `{}` is not a file", self.index.display()),
        }
    }
}

impl StdError for NoIndex {
    fn source(&self) -> Option<&(dyn StdError + 'static)> {
        self.source.as_ref().map(|error| error as _)
    }
}

impl Page {
    /// The answer to `request`: refused by the filter, or `ServeDir`'s, with the headers of the page.
    async fn answer(mut self, request: Request) -> Response {
        if request.method() != Method::GET && request.method() != Method::HEAD {
            return method_not_allowed();
        }

        let path = request.uri().path();
        let segments = match segments(path) {
            Ok(segments) => segments,
            Err(refused) => return Error::not_found(refused.detail()).into_response(),
        };
        let relative = segments.join("/");
        let slashed = path.ends_with('/');

        let dir = Arc::clone(&self.dir);
        let manifest = Arc::clone(&self.manifest);
        let checked = {
            let relative = relative.clone();

            tokio::task::spawn_blocking(move || -> Result<Hashed, Refusal> {
                filter(&dir, &relative, slashed)?;

                Ok(dir
                    .canonicalize()
                    .map_or(Hashed::Named, |root| manifest.hashed(&root)))
            })
            .await
        };
        let hashed = match checked {
            Ok(Ok(hashed)) => hashed,
            Ok(Err(refusal)) => return refusal.into_response(),
            Err(error) => return Error::internal(error).into_response(),
        };

        // `ServeDir` is ready when its fallback is, which always is.
        let _ = poll_fn(|cx| self.files.poll_ready(cx)).await;

        let served = match self.files.call(request).await {
            Ok(served) => served.map(Body::new),
            Err(never) => match never {},
        };

        dressed(served, &relative, &hashed)
    }
}

/// The 405 of a method other than `GET` and `HEAD`.
fn method_not_allowed() -> Response {
    let mut response = Error::new(
        StatusCode::METHOD_NOT_ALLOWED,
        "the page answers `GET` and `HEAD` alone",
    )
    .into_response();

    response
        .headers_mut()
        .insert(header::ALLOW, HeaderValue::from_static("GET, HEAD"));
    response
}

/// `served`, an answer of `ServeDir`, of the file at `relative`, with the `Cache-Control` of a file that `hashed`
/// tells, and a problem for an error it answered with no body.
fn dressed(served: Response, relative: &str, hashed: &Hashed) -> Response {
    let status = served.status();
    let mut served = if status.is_server_error() {
        Error::internal("the page's file cannot be read").into_response()
    } else if status == StatusCode::PRECONDITION_FAILED {
        Error::new(
            status,
            "the file was modified since the date the request gives",
        )
        .into_response()
    } else {
        served
    };
    let headers = served.headers_mut();

    // The fallback gives its own; an error none.
    let cached = status.is_success() || status == StatusCode::NOT_MODIFIED;

    if cached && !headers.contains_key(header::CACHE_CONTROL) {
        let cache = if hashed.names(relative) {
            IMMUTABLE
        } else {
            NO_CACHE
        };

        headers.insert(header::CACHE_CONTROL, cache);
    }

    served
}

/// What `ServeDir` falls back to when a path is no file: `index.html` for a browser's request of a page, a problem
/// 404 for any other request.
#[derive(Debug, Clone)]
struct Fallback {
    dir: Arc<Path>,
}

impl Service<Request> for Fallback {
    type Response = Response;
    type Error = Infallible;
    type Future = Pin<Box<dyn Future<Output = Result<Response, Infallible>> + Send>>;

    fn poll_ready(&mut self, _cx: &mut Context<'_>) -> Poll<Result<(), Infallible>> {
        Poll::Ready(Ok(()))
    }

    fn call(&mut self, request: Request) -> Self::Future {
        let dir = Arc::clone(&self.dir);

        Box::pin(async move { Ok(fall_back(dir, request).await) })
    }
}

/// The answer to `request`, of a path that is no file of `dir`.
async fn fall_back(dir: Arc<Path>, request: Request) -> Response {
    // Told by the decoded segments, as `/%2Ewell-known/` and `/./.well-known/` are `/.well-known/` too.
    let well_known = segments(request.uri().path()).map_or(true, |segments| {
        segments.first().is_some_and(|first| first == WELL_KNOWN)
    });

    // Programs read the files of `/.well-known/`, which are never a page.
    if well_known {
        return Error::not_found("the page has no file of this path").into_response();
    }

    if !accepts_html(request.headers()) {
        let mut response = Error::not_found("the page has no file of this path").into_response();

        // A request that accepted `text/html` would have had `index.html`.
        vary(response.headers_mut(), "accept");
        return response;
    }

    let checked = {
        let dir = Arc::clone(&dir);

        tokio::task::spawn_blocking(move || filter(&dir, "", true)).await
    };
    match checked {
        Ok(Ok(())) => {}
        Ok(Err(refusal)) => return refusal.into_response(),
        Err(error) => return Error::internal(error).into_response(),
    }

    let mut index = ServeFile::new(dir.join(INDEX))
        .precompressed_br()
        .precompressed_gzip()
        .precompressed_zstd();
    let mut served = match index.call(request).await {
        Ok(served) => served.map(Body::new),
        Err(never) => match never {},
    };

    if served.status() == StatusCode::NOT_FOUND {
        return Error::not_found("the page has no `index.html`").into_response();
    }

    let headers = served.headers_mut();

    headers.insert(header::CACHE_CONTROL, NO_CACHE);
    vary(headers, "accept");
    served
}

/// Why a path of a request is refused, each a 404.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Refused {
    /// A `..` segment, or a symbolic link whose target is out of the directory.
    Outside,
    /// A segment that begins with a `.`, a hidden file or directory, but a first `.well-known`.
    Hidden,
    /// A path that is not UTF-8 once decoded.
    NotUtf8,
    /// A segment with a `\` or a NUL, or empty, which no segment of a file's path is.
    Unnamable,
}

impl Refused {
    /// The detail of its 404.
    const fn detail(self) -> &'static str {
        match self {
            Self::Outside => "the path leads out of the page's directory",
            Self::Hidden => "the page serves no hidden file, whose name begins with a `.`",
            Self::NotUtf8 => {
                "the path is not UTF-8 once decoded, which no file of the page is named in"
            }
            Self::Unnamable => {
                "the path holds an empty segment, a `\\` or a NUL, which no file of the page is named with"
            }
        }
    }
}

/// The segments of a request's `path`, decoded, or why it is refused. An empty segment is refused but at the ends,
/// `/` and `/docs/`, so that no path of a redirection `ServeDir` writes begins with `//`, another host.
fn segments(path: &str) -> Result<Vec<String>, Refused> {
    let decoded = percent_decode_str(path)
        .decode_utf8()
        .map_err(|_| Refused::NotUtf8)?;
    let parts: Vec<&str> = decoded.split('/').collect();
    let last = parts.len().saturating_sub(1);
    let mut segments: Vec<String> = Vec::new();

    for (at, segment) in parts.into_iter().enumerate() {
        match segment {
            "" if at == 0 || at == last => {}
            "" => return Err(Refused::Unnamable),
            "." => {}
            ".." => return Err(Refused::Outside),
            _ if segment.contains(['\\', '\0']) => return Err(Refused::Unnamable),
            WELL_KNOWN if segments.is_empty() => segments.push(segment.to_owned()),
            _ if segment.starts_with('.') => return Err(Refused::Hidden),
            _ => segments.push(segment.to_owned()),
        }
    }

    Ok(segments)
}

/// Why the filter refuses a request before `ServeDir` reads it.
#[derive(Debug, PartialEq, Eq)]
enum Refusal {
    /// A file the page does not serve, by its real path.
    Refused(Refused),
    /// A file whose time no HTTP date holds, before 1970 or from the year 10000, on which `ServeDir` panics.
    Undated(PathBuf),
}

impl IntoResponse for Refusal {
    fn into_response(self) -> Response {
        match self {
            Self::Refused(refused) => Error::not_found(refused.detail()).into_response(),
            Self::Undated(file) => Error::internal(format!(
                "the page's file `{}` has a time that no HTTP date holds, before 1970 or from the year 10000: touch it",
                file.display()
            ))
            .into_response(),
        }
    }
}

/// The filter of a request of the path of `relative`, the decoded segments joined by `/`, under `dir`, `slashed` when
/// the request's path ends with a `/`. It finds the file `ServeDir` will open, as `ServeDir` does, then checks it and
/// each compressed copy of it that `ServeDir` may send by their real paths, which must be under `dir`'s and not
/// hidden, and checks that the time of each that is a file is one an HTTP date holds. Nothing is opened. A file that
/// is not there, or that the server cannot read, is left to `ServeDir`, which falls back.
///
/// As `ServeDir` does, a directory opens its `index.html` when the path ends with a `/`, and is redirected otherwise;
/// any other path opens the file of its segments, whatever its final `/`.
fn filter(dir: &Path, relative: &str, slashed: bool) -> Result<(), Refusal> {
    let Ok(root) = dir.canonicalize() else {
        return Ok(());
    };
    let mut target = root.join(relative);

    if std::fs::metadata(&target).is_ok_and(|metadata| metadata.is_dir()) {
        if !slashed {
            return Ok(());
        }

        target.push(INDEX);
    }

    let copies = COPIES.map(|extension| {
        let mut copy = target.clone().into_os_string();

        copy.push(".");
        copy.push(extension);
        PathBuf::from(copy)
    });

    for candidate in std::iter::once(target).chain(copies) {
        let Ok(real) = candidate.canonicalize() else {
            continue;
        };

        under(&root, &real).map_err(Refusal::Refused)?;

        let undated = std::fs::metadata(&real)
            .ok()
            .filter(Metadata::is_file)
            .and_then(|metadata| metadata.modified().ok())
            .is_some_and(|modified| !is_http_date(modified));

        if undated {
            return Err(Refusal::Undated(candidate));
        }
    }

    Ok(())
}

/// Whether `real`, a real path, is a file the page serves under `root`, its real path: under it, and with no hidden
/// component below it, whose name begins with a `.`, but a first `.well-known`. A symbolic link to `.git`, or a short
/// name of Windows, `GIT~1`, is told by the real path it leads to.
fn under(root: &Path, real: &Path) -> Result<(), Refused> {
    let Ok(below) = real.strip_prefix(root) else {
        return Err(Refused::Outside);
    };

    for (at, component) in below.components().enumerate() {
        let Component::Normal(name) = component else {
            return Err(Refused::Outside);
        };
        let hidden = name.as_encoded_bytes().starts_with(b".");

        if hidden && !(at == 0 && name == WELL_KNOWN) {
            return Err(Refused::Hidden);
        }
    }

    Ok(())
}

/// The first second an HTTP date cannot hold, in the year 10000.
const YEAR_10000: u64 = 253_402_300_800;

/// Whether an HTTP date holds `time`: from 1970, and before the year 10000.
fn is_http_date(time: SystemTime) -> bool {
    time.duration_since(UNIX_EPOCH)
        .is_ok_and(|since| since.as_secs() < YEAR_10000)
}

/// Whether `headers` accept `text/html`, as a browser's request of a page does, and a script's or an image's does
/// not: the request that `index.html` answers when its path is no file. `*/*` alone does not count.
fn accepts_html(headers: &HeaderMap) -> bool {
    headers
        .get_all(header::ACCEPT)
        .iter()
        .filter_map(|value| value.to_str().ok())
        .flat_map(|value| value.split(','))
        .any(|range| {
            let mut parts = range.split(';');
            let media_type = parts.next().unwrap_or_default().trim();

            media_type.eq_ignore_ascii_case("text/html") && weight(parts) > 0.0
        })
}

/// The weight of an item of `Accept`, from its parameters: its `q`, 1 when it has none, 0 when it does not read.
fn weight<'a>(parameters: impl Iterator<Item = &'a str>) -> f32 {
    parameters
        .filter_map(|parameter| {
            let parameter = parameter.trim();

            parameter
                .strip_prefix("q=")
                .or_else(|| parameter.strip_prefix("Q="))
        })
        .map(|weight| weight.trim().parse::<f32>().unwrap_or(0.0))
        .next()
        .unwrap_or(1.0)
}

/// The names of the files Vite hashed, read from its manifest, again when the manifest changes.
#[derive(Debug, Default)]
struct Manifest {
    read: Mutex<Option<Read>>,
}

/// A manifest read: what it was on disk, and the files it names, `None` when it does not read.
#[derive(Debug)]
struct Read {
    stamp: Stamp,
    files: Option<Arc<HashSet<String>>>,
}

/// What a file is on disk, which changes when it is written anew: its length, the time it was modified, and on Unix
/// its device, its inode and the time its inode changed, which a build that fixes the times of its files,
/// `SOURCE_DATE_EPOCH`, still changes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct Stamp {
    len: u64,
    modified: Option<SystemTime>,
    #[cfg(unix)]
    inode: (u64, u64, i64, i64),
}

impl Stamp {
    fn of(metadata: &Metadata) -> Self {
        #[cfg(unix)]
        let inode = {
            use std::os::unix::fs::MetadataExt;

            (
                metadata.dev(),
                metadata.ino(),
                metadata.ctime(),
                metadata.ctime_nsec(),
            )
        };

        Self {
            len: metadata.len(),
            modified: metadata.modified().ok(),
            #[cfg(unix)]
            inode,
        }
    }
}

/// How the files named by their hash are told.
#[derive(Debug)]
enum Hashed {
    /// By the manifest of Vite, which names them.
    Listed(Arc<HashSet<String>>),
    /// By their names alone, with no manifest.
    Named,
}

impl Hashed {
    /// Whether the file at `path`, under the page's directory, is named by its hash: its name has the shape of one,
    /// and the manifest names it, or the file it is the `.map` of. With no manifest, the file is under `assets/` and
    /// its hash holds a digit, which a word does not: `assets/logo-Darkmode.svg` is not one.
    fn names(&self, path: &str) -> bool {
        match self {
            Self::Listed(files) => {
                let listed = files.contains(path)
                    || path
                        .strip_suffix(".map")
                        .is_some_and(|file| files.contains(file));

                listed && hashes(path).next().is_some()
            }
            Self::Named => {
                path.strip_prefix(ASSETS)
                    .is_some_and(|rest| rest.starts_with('/'))
                    && hashes(path).any(|hash| hash.iter().any(u8::is_ascii_digit))
            }
        }
    }
}

impl Manifest {
    /// How the files under `root`, a real path, named by their hash are told: by `root/.vite/manifest.json` when it
    /// reads, else by their names. It is read again when its [`Stamp`] changes.
    fn hashed(&self, root: &Path) -> Hashed {
        let real = root
            .join(MANIFEST)
            .canonicalize()
            .ok()
            .filter(|real| real.starts_with(root));
        let metadata = real
            .as_ref()
            .and_then(|real| std::fs::metadata(real).ok())
            .filter(Metadata::is_file);
        let mut read = self.read.lock().unwrap_or_else(PoisonError::into_inner);

        let (Some(real), Some(metadata)) = (real, metadata) else {
            *read = None;
            return Hashed::Named;
        };

        let stamp = Stamp::of(&metadata);

        if read.as_ref().is_none_or(|read| read.stamp != stamp) {
            let files = std::fs::read(&real)
                .map_err(|error| error.to_string())
                .and_then(|bytes| listed(&bytes).map_err(|error| error.to_string()));
            let files = match files {
                Ok(files) => Some(Arc::new(files)),
                Err(error) => {
                    tracing::warn!(
                        manifest = %real.display(),
                        error = %error,
                        "the page's Vite manifest does not read: its hashed files are told by their names"
                    );
                    None
                }
            };

            *read = Some(Read { stamp, files });
        }

        match read.as_ref().and_then(|read| read.files.clone()) {
            Some(files) => Hashed::Listed(files),
            None => Hashed::Named,
        }
    }
}

/// The files a manifest of Vite names, the `file`, `css` and `assets` of each of its chunks.
///
/// # Errors
///
/// When `bytes` are not a JSON object.
fn listed(bytes: &[u8]) -> Result<HashSet<String>, serde_json::Error> {
    let manifest: serde_json::Map<String, serde_json::Value> = serde_json::from_slice(bytes)?;
    let mut files = HashSet::new();

    for chunk in manifest.values() {
        if let Some(file) = chunk.get("file").and_then(serde_json::Value::as_str) {
            files.insert(file.to_owned());
        }

        for key in ["css", "assets"] {
            let listed = chunk.get(key).and_then(serde_json::Value::as_array);

            for file in listed.into_iter().flatten() {
                if let Some(file) = file.as_str() {
                    files.insert(file.to_owned());
                }
            }
        }
    }

    Ok(files)
}

/// The eight characters of each place where the name of the file at `path` has the shape of a hash, as Vite writes
/// one: a `-`, eight characters of base64url, and a `.`, `index-BXa3kD9f.js`.
fn hashes(path: &str) -> impl Iterator<Item = &[u8]> {
    let name = path.rsplit('/').next().unwrap_or(path).as_bytes();

    (0..name.len()).filter_map(move |at| {
        let hash = name.get(at + 1..at + 9)?;
        let shaped = name[at] == b'-'
            && name.get(at + 9) == Some(&b'.')
            && hash
                .iter()
                .all(|&byte| byte.is_ascii_alphanumeric() || byte == b'-' || byte == b'_');

        shaped.then_some(hash)
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn is_hashed(path: &str) -> bool {
        Hashed::Named.names(path)
    }

    #[test]
    fn with_no_manifest_a_name_vite_writes_is_hashed() {
        assert!(is_hashed("assets/index-BXa3kD9f.js"));
        assert!(is_hashed("assets/index-B-3fk9aZ.css"));
        assert!(is_hashed("assets/vendor.lib-abc_1234.js"));
        assert!(is_hashed("assets/index-BXa3kD9f.js.map"));
        assert!(is_hashed("assets/fonts/inter-x1y2z3w4.woff2"));
    }

    #[test]
    fn with_no_manifest_any_other_name_is_not() {
        assert!(!is_hashed("index.html"));
        assert!(!is_hashed("favicon-BXa3kD9f.ico"));
        assert!(!is_hashed("assets/logo.svg"));
        assert!(!is_hashed("assets/my-elements.js"));
        assert!(!is_hashed("assets/logo-Darkmode.svg"));
        assert!(!is_hashed("assets/icon-Dark_Mod.svg"));
        assert!(!is_hashed("assets/index-BXa3kD9.js"));
        assert!(!is_hashed("assets/index-BXa3kD9fg.js"));
        assert!(!is_hashed("assetsx/index-BXa3kD9f.js"));
        assert!(!is_hashed("assets/é-BXa3kD9f"));
    }

    #[test]
    fn with_a_manifest_a_hash_must_both_have_its_shape_and_be_listed() {
        let files = [
            "assets/index-abcdefgh.js",
            "assets/logo.svg",
            "static/app-Q-_xYzWv.css",
        ];
        let hashed = Hashed::Listed(Arc::new(files.map(str::to_owned).into()));

        // A hash of no digit is one when the manifest names it, wherever Vite writes it.
        assert!(hashed.names("assets/index-abcdefgh.js"));
        assert!(hashed.names("assets/index-abcdefgh.js.map"));
        assert!(hashed.names("static/app-Q-_xYzWv.css"));
        // Listed with no hash in its name: not kept a year.
        assert!(!hashed.names("assets/logo.svg"));
        // Shaped as a hash, but not listed.
        assert!(!hashed.names("assets/index-BXa3kD9f.js"));
        assert!(!hashed.names("assets/other-abcdefgh.js.map"));
    }

    #[test]
    fn a_segment_out_of_the_directory_is_refused() {
        assert_eq!(segments("/a/../b"), Err(Refused::Outside));
        assert_eq!(segments("/a/%2e%2e/b"), Err(Refused::Outside));
        assert_eq!(segments("/a%2F..%2Fb"), Err(Refused::Outside));
        assert_eq!(segments("/a\\b"), Err(Refused::Unnamable));
        assert_eq!(segments("/a%00b"), Err(Refused::Unnamable));
        assert_eq!(
            segments("/assets/./app.js"),
            Ok(vec!["assets".to_owned(), "app.js".to_owned()])
        );
    }

    #[test]
    fn an_empty_segment_is_refused_but_at_the_ends() {
        assert_eq!(segments("/"), Ok(Vec::new()));
        assert_eq!(segments("/docs/"), Ok(vec!["docs".to_owned()]));
        // `ServeDir` would redirect `//evil.example`, a directory, to `//evil.example/`, another host.
        assert_eq!(segments("//evil.example"), Err(Refused::Unnamable));
        assert_eq!(segments("/assets//app.js"), Err(Refused::Unnamable));
        assert_eq!(segments("/a%2F%2Fb"), Err(Refused::Unnamable));
    }

    #[test]
    fn a_hidden_segment_is_refused_but_a_first_well_known() {
        assert_eq!(segments("/.env"), Err(Refused::Hidden));
        assert_eq!(segments("/.vite/manifest.json"), Err(Refused::Hidden));
        assert_eq!(segments("/assets/.git/config"), Err(Refused::Hidden));
        assert_eq!(segments("/%2Egit/config"), Err(Refused::Hidden));
        assert_eq!(segments("/a/.well-known/x"), Err(Refused::Hidden));
        assert_eq!(
            segments("/.well-known/security.txt"),
            Ok(vec![".well-known".to_owned(), "security.txt".to_owned()])
        );
    }

    #[test]
    fn a_path_not_utf8_has_its_own_refusal() {
        assert_eq!(segments("/%FF.js"), Err(Refused::NotUtf8));
        assert_ne!(Refused::NotUtf8.detail(), Refused::Outside.detail());
    }

    #[test]
    fn a_request_of_a_page_accepts_html() {
        let accepts = |value: &'static str| {
            let mut headers = HeaderMap::new();

            headers.insert(header::ACCEPT, HeaderValue::from_static(value));
            accepts_html(&headers)
        };

        assert!(accepts(
            "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"
        ));
        assert!(accepts("TEXT/HTML; q=0.5"));
        assert!(!accepts("*/*"));
        assert!(!accepts("text/css,*/*;q=0.1"));
        assert!(!accepts("text/html;q=0"));
        assert!(!accepts_html(&HeaderMap::new()));
    }

    #[test]
    fn a_real_path_below_the_root_must_be_neither_out_of_it_nor_hidden() {
        let root = Path::new("/srv/dist");

        assert_eq!(under(root, Path::new("/srv/dist/assets/app.js")), Ok(()));
        assert_eq!(
            under(root, Path::new("/srv/dist/.well-known/security.txt")),
            Ok(())
        );
        assert_eq!(
            under(root, Path::new("/srv/secret.txt")),
            Err(Refused::Outside)
        );
        assert_eq!(
            under(root, Path::new("/srv/dist/.git/config")),
            Err(Refused::Hidden)
        );
        assert_eq!(
            under(root, Path::new("/srv/dist/a/.well-known/x")),
            Err(Refused::Hidden)
        );
        assert_eq!(
            under(root, Path::new("/srv/dist/assets/.env")),
            Err(Refused::Hidden)
        );
    }

    #[test]
    fn a_manifest_names_the_files_of_its_chunks() {
        let manifest = br#"{
            "index.html": {
                "file": "assets/index-abcdefgh.js",
                "src": "index.html",
                "isEntry": true,
                "css": ["assets/index-ABCDEFGH.css"],
                "assets": ["assets/logo-Darkmode.svg"]
            },
            "_shared-x.js": { "file": "assets/shared-x1y2z3w4.js" }
        }"#;
        let files = listed(manifest).expect("a manifest");

        assert_eq!(files.len(), 4);
        assert!(files.contains("assets/index-abcdefgh.js"));
        assert!(files.contains("assets/index-ABCDEFGH.css"));
        assert!(files.contains("assets/logo-Darkmode.svg"));
        assert!(!files.contains("index.html"));
        assert!(listed(b"[1, 2]").is_err());
        assert!(listed(b"not json").is_err());
    }

    #[test]
    fn a_name_is_added_to_vary_once() {
        let mut headers = HeaderMap::new();

        vary(&mut headers, "accept-encoding");
        vary(&mut headers, "accept-encoding");
        assert_eq!(headers.get_all(header::VARY).iter().count(), 1);
        assert_eq!(headers[header::VARY], "accept-encoding");

        headers.append(header::VARY, HeaderValue::from_static("Accept, origin"));
        vary(&mut headers, "accept");
        assert_eq!(headers.get_all(header::VARY).iter().count(), 1);
        assert_eq!(headers[header::VARY], "accept-encoding, accept, origin");
    }

    #[test]
    fn only_the_times_an_http_date_holds_are_dated() {
        let year_10000 = UNIX_EPOCH + std::time::Duration::from_secs(YEAR_10000);

        assert!(is_http_date(UNIX_EPOCH));
        assert!(is_http_date(year_10000 - std::time::Duration::from_secs(1)));
        assert!(!is_http_date(
            UNIX_EPOCH - std::time::Duration::from_secs(1)
        ));
        assert!(!is_http_date(year_10000));
    }

    #[cfg(unix)]
    #[test]
    fn the_filter_reads_each_copy_by_its_real_path() {
        let outer = tempfile::tempdir().expect("a temporary directory is made");
        let dist = outer.path().join("dist");

        std::fs::create_dir(&dist).expect("the directory is made");
        std::fs::write(dist.join("app.js"), "inside").expect("the file is written");
        std::fs::write(dist.join("index.html"), "index").expect("the file is written");
        std::fs::write(outer.path().join("secret"), "secret").expect("the file is written");
        std::os::unix::fs::symlink(outer.path().join("secret"), dist.join("app.js.gz"))
            .expect("the link is made");

        assert_eq!(
            filter(&dist, "app.js", false),
            Err(Refusal::Refused(Refused::Outside))
        );
        // `ServeDir` opens a file whatever its final `/`.
        assert_eq!(
            filter(&dist, "app.js", true),
            Err(Refusal::Refused(Refused::Outside))
        );
        assert_eq!(filter(&dist, "", true), Ok(()));
        assert_eq!(filter(&dist, "gone.js", false), Ok(()));
    }
}
