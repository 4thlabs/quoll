//! The page a bundler builds, served from its directory beside the api: each file of the directory, and `index.html`
//! for any other path, so that the page's router takes it.

use std::convert::Infallible;
use std::error::Error as StdError;
use std::fmt;
use std::fs::{File, Metadata};
use std::future::Future;
use std::io;
use std::path::{Path, PathBuf};
use std::pin::Pin;
use std::sync::Arc;
use std::task::{Context, Poll};
use std::time::{SystemTime, UNIX_EPOCH};

use axum::body::Body;
use axum::extract::Request;
use axum::response::{IntoResponse, Response};
use http::{HeaderMap, HeaderValue, Method, StatusCode, header};
use percent_encoding::percent_decode_str;
use tokio::io::AsyncReadExt;
use tokio_util::io::ReaderStream;
use tower_service::Service;

use crate::Error;

/// The `Cache-Control` of a file the bundler names by its hash: its content never changes under its name.
const IMMUTABLE: HeaderValue = HeaderValue::from_static("public, max-age=31536000, immutable");

/// The `Cache-Control` of any other file: kept, and checked again before each use.
const NO_CACHE: HeaderValue = HeaderValue::from_static("no-cache");

/// The file answered at `/`, and at any path that is no file.
const INDEX: &str = "index.html";

/// The directory of the files the bundler names by their hash, as Vite names it.
const ASSETS: &str = "assets";

/// The files of a page's directory, a service an axum router falls back to.
#[derive(Debug, Clone)]
pub(crate) struct Page {
    dir: Arc<Path>,
}

impl Page {
    /// The page of the files in `dir`, read at each request.
    pub(crate) fn new(dir: PathBuf) -> Self {
        Self { dir: dir.into() }
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
        let dir = Arc::clone(&self.dir);

        Box::pin(async move { Ok(answer(dir, request).await) })
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

/// The answer to `request` from the files of `dir`.
async fn answer(dir: Arc<Path>, request: Request) -> Response {
    let (parts, _) = request.into_parts();

    if parts.method != Method::GET && parts.method != Method::HEAD {
        let mut response = Error::new(
            StatusCode::METHOD_NOT_ALLOWED,
            "the page answers `GET` and `HEAD` alone",
        )
        .into_response();

        response
            .headers_mut()
            .insert(header::ALLOW, HeaderValue::from_static("GET, HEAD"));
        return response;
    }

    let segments = match segments(parts.uri.path()) {
        Ok(segments) => segments,
        Err(refused) => return Error::not_found(refused.detail()).into_response(),
    };
    let encodings = accepted(&parts.headers);
    let found = tokio::task::spawn_blocking(move || find(&dir, &segments, &encodings)).await;

    match found {
        Ok(Opened::Yes(served)) => served.respond(&parts.headers, parts.method == Method::HEAD),
        Ok(Opened::Outside) => Error::not_found(Refused::Outside.detail()).into_response(),
        Ok(Opened::No) => Error::not_found("the page has no file of this path").into_response(),
        Err(error) => Error::internal(error).into_response(),
    }
}

/// Why a path of a request is refused before any file is looked for, each a 404.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Refused {
    /// A `..` segment, or a symbolic link whose target is out of the directory.
    Outside,
    /// A segment that begins with a `.`, a hidden file or directory, but a first `.well-known`.
    Hidden,
    /// A path that is not UTF-8 once decoded.
    NotUtf8,
    /// A segment with a `\` or a NUL, which no segment of a file's path holds.
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
                "the path holds a `\\` or a NUL, which no file of the page is named with"
            }
        }
    }
}

/// The well-known directory of RFC 8615, the one hidden directory a page serves, at its root.
const WELL_KNOWN: &str = ".well-known";

/// The segments of a request's `path`, decoded, or why it is refused.
fn segments(path: &str) -> Result<Vec<String>, Refused> {
    let decoded = percent_decode_str(path)
        .decode_utf8()
        .map_err(|_| Refused::NotUtf8)?;
    let mut segments: Vec<String> = Vec::new();

    for segment in decoded.split('/') {
        match segment {
            "" | "." => {}
            ".." => return Err(Refused::Outside),
            _ if segment.contains(['\\', '\0']) => return Err(Refused::Unnamable),
            WELL_KNOWN if segments.is_empty() => segments.push(segment.to_owned()),
            _ if segment.starts_with('.') => return Err(Refused::Hidden),
            _ => segments.push(segment.to_owned()),
        }
    }

    Ok(segments)
}

/// Whether a path of `segments` that is no file answers `index.html`: a path that looks like one of the page's
/// routes. A path under `assets/`, or whose last segment has an extension, names a file: a 404 tells a missing one,
/// where `index.html` would be read as a script or a style.
fn is_route(segments: &[String]) -> bool {
    let Some(last) = segments.last() else {
        return true;
    };

    segments[0] != ASSETS && !last.contains('.')
}

/// A content coding a file may be stored in beside itself, `app.js.br` beside `app.js`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Encoding {
    Brotli,
    Gzip,
}

impl Encoding {
    /// The extension of the file stored in it.
    const fn extension(self) -> &'static str {
        match self {
            Self::Brotli => "br",
            Self::Gzip => "gz",
        }
    }

    /// Its name in `Content-Encoding`.
    const fn name(self) -> &'static str {
        match self {
            Self::Brotli => "br",
            Self::Gzip => "gzip",
        }
    }
}

/// The codings `headers` accept, the most wanted first, Brotli before gzip when they are wanted alike.
fn accepted(headers: &HeaderMap) -> Vec<Encoding> {
    let mut brotli = None;
    let mut gzip = None;
    let mut any = None;

    let codings = headers
        .get_all(header::ACCEPT_ENCODING)
        .iter()
        .filter_map(|value| value.to_str().ok())
        .flat_map(|value| value.split(','));

    for coding in codings {
        let mut parts = coding.split(';');
        let name = parts.next().unwrap_or_default().trim();
        let weight = parts
            .filter_map(|part| {
                part.trim()
                    .strip_prefix("q=")
                    .or(part.trim().strip_prefix("Q="))
            })
            .map(|weight| weight.trim().parse::<f32>().unwrap_or(0.0))
            .next()
            .unwrap_or(1.0);

        if name.eq_ignore_ascii_case("br") {
            brotli = Some(weight);
        } else if name.eq_ignore_ascii_case("gzip") || name.eq_ignore_ascii_case("x-gzip") {
            gzip = Some(weight);
        } else if name == "*" {
            any = Some(weight);
        }
    }

    let brotli = brotli.or(any).unwrap_or(0.0);
    let gzip = gzip.or(any).unwrap_or(0.0);
    let mut encodings = Vec::new();

    if brotli > 0.0 {
        encodings.push((Encoding::Brotli, brotli));
    }

    if gzip > 0.0 {
        encodings.push((Encoding::Gzip, gzip));
    }

    // A stable sort keeps Brotli first when both weigh the same.
    encodings.sort_by(|(_, first), (_, second)| second.total_cmp(first));
    encodings
        .into_iter()
        .map(|(encoding, _)| encoding)
        .collect()
}

/// The file at `segments` under `dir`, opened in the first of `encodings` stored beside it: the file of that path,
/// else the `index.html` of a directory of that path, else `dir`'s `index.html` for a path that looks like a route.
/// Each is opened by its real path, its symbolic links followed, which must be in `dir`'s.
fn find(dir: &Path, segments: &[String], encodings: &[Encoding]) -> Opened<Served> {
    let Ok(root) = dir.canonicalize() else {
        return Opened::No;
    };

    if segments.is_empty() {
        return served(&root, INDEX, encodings);
    }

    let path = segments.join("/");

    match served(&root, &path, encodings) {
        Opened::No => {}
        opened => return opened,
    }

    // A file of that path is no directory, whose `index.html` is not opened then.
    match served(&root, &format!("{path}/{INDEX}"), encodings) {
        Opened::No if is_route(segments) => served(&root, INDEX, encodings),
        opened => opened,
    }
}

/// What opening a file gives.
enum Opened<T> {
    /// The file.
    Yes(T),
    /// A file out of the directory, through a symbolic link.
    Outside,
    /// No file.
    No,
}

/// The file at `path` under `root`, in the first of `encodings` stored beside it, or as it is.
fn served(root: &Path, path: &str, encodings: &[Encoding]) -> Opened<Served> {
    let plain = root.join(path);
    let (file, metadata) = match open(root, &plain) {
        Opened::Yes(opened) => opened,
        Opened::Outside => return Opened::Outside,
        Opened::No => return Opened::No,
    };

    // A coding stored out of the directory, or that cannot be read, is passed over.
    for &encoding in encodings {
        let mut stored = plain.clone().into_os_string();

        stored.push(".");
        stored.push(encoding.extension());

        if let Opened::Yes((file, metadata)) = open(root, Path::new(&stored)) {
            return Opened::Yes(Served {
                path: path.to_owned(),
                file,
                metadata,
                encoding: Some(encoding),
            });
        }
    }

    Opened::Yes(Served {
        path: path.to_owned(),
        file,
        metadata,
        encoding: None,
    })
}

/// The regular file at `path`, by its real path, which must be under `root`, itself a real path.
fn open(root: &Path, path: &Path) -> Opened<(File, Metadata)> {
    let Ok(real) = path.canonicalize() else {
        return Opened::No;
    };

    if !real.starts_with(root) {
        return Opened::Outside;
    }

    let Ok(file) = File::open(&real) else {
        return Opened::No;
    };
    let Ok(metadata) = file.metadata() else {
        return Opened::No;
    };

    if !metadata.is_file() {
        return Opened::No;
    }

    if !is_still_under(root, &real, &file, &metadata) {
        return Opened::Outside;
    }

    Opened::Yes((file, metadata))
}

/// Whether `file`, opened at `real`, a real path under `root`, is still under it: a directory of the path swapped
/// for a symbolic link between the check and the open would have opened a file elsewhere.
///
/// On Linux, the real path of the opened file is read through `/proc/self/fd`. Elsewhere, or without `/proc`, the
/// path is made real again, and on Unix must still lead to the same file, by its device and inode: a link swapped in
/// and out again between the two checks is not seen.
fn is_still_under(root: &Path, real: &Path, file: &File, metadata: &Metadata) -> bool {
    #[cfg(target_os = "linux")]
    {
        use std::os::fd::AsRawFd;

        if let Ok(opened) = std::fs::read_link(format!("/proc/self/fd/{}", file.as_raw_fd())) {
            return opened.starts_with(root);
        }
    }

    #[cfg(not(target_os = "linux"))]
    let _ = file;

    let Ok(again) = real.canonicalize() else {
        return false;
    };

    again.starts_with(root) && is_same_file(&again, metadata)
}

/// Whether the file at `path` is the one of `metadata`, by its device and inode.
#[cfg(unix)]
fn is_same_file(path: &Path, metadata: &Metadata) -> bool {
    use std::os::unix::fs::MetadataExt;

    std::fs::metadata(path)
        .is_ok_and(|other| other.dev() == metadata.dev() && other.ino() == metadata.ino())
}

/// Windows names a file by no stable number: the path made real again is the check.
#[cfg(not(unix))]
fn is_same_file(_path: &Path, _metadata: &Metadata) -> bool {
    true
}

/// A file of the page, opened, to be answered.
struct Served {
    /// Its path under the directory, `/` between its segments, which names its type and its cache.
    path: String,
    file: File,
    metadata: Metadata,
    /// The coding it is stored in, or `None` as it is.
    encoding: Option<Encoding>,
}

impl Served {
    /// The answer of the file to a request of `headers`: a 304 when the request's copy is the file's, else the file,
    /// with no body for a `HEAD`.
    fn respond(self, headers: &HeaderMap, head: bool) -> Response {
        // A time an HTTP date cannot hold is no time: no `Last-Modified`, and `If-Modified-Since` is not read.
        let modified = self
            .metadata
            .modified()
            .ok()
            .filter(|modified| http_date(*modified).is_some());
        let tag = self.tag(modified);
        let mut response = Response::new(Body::empty());
        let answered = response.headers_mut();

        answered.insert(header::ETAG, tag.clone());
        answered.insert(header::CACHE_CONTROL, self.cache());
        answered.insert(header::VARY, HeaderValue::from_static("accept-encoding"));
        answered.insert(
            header::X_CONTENT_TYPE_OPTIONS,
            HeaderValue::from_static("nosniff"),
        );

        if let Some(date) = modified.and_then(http_date) {
            answered.insert(header::LAST_MODIFIED, date);
        }

        if not_modified(headers, &tag, modified) {
            *response.status_mut() = StatusCode::NOT_MODIFIED;
            return response;
        }

        answered.insert(header::CONTENT_TYPE, self.content_type());
        answered.insert(header::CONTENT_LENGTH, self.metadata.len().into());

        if let Some(encoding) = self.encoding {
            answered.insert(
                header::CONTENT_ENCODING,
                HeaderValue::from_static(encoding.name()),
            );
        }

        // No more than its `Content-Length`, though the file grows as it is sent.
        if !head {
            let file = tokio::fs::File::from_std(self.file).take(self.metadata.len());

            *response.body_mut() = Body::from_stream(ReaderStream::new(file));
        }

        response
    }

    /// Its entity tag: its length, the time it was modified and its coding. It is weak, as a layer may compress a
    /// file stored as it is, which keeps its tag.
    fn tag(&self, modified: Option<SystemTime>) -> HeaderValue {
        let nanos = modified
            .and_then(|modified| modified.duration_since(UNIX_EPOCH).ok())
            .map_or(0, |since| since.as_nanos());
        let coding = self.encoding.map_or(String::new(), |encoding| {
            format!("-{}", encoding.extension())
        });
        let tag = format!("W/\"{:x}-{nanos:x}{coding}\"", self.metadata.len());

        HeaderValue::from_str(&tag).expect("a tag of hexadecimal digits is a header value")
    }

    /// Its `Cache-Control`: kept a year when the bundler names it by its hash, checked again otherwise.
    fn cache(&self) -> HeaderValue {
        if is_hashed(&self.path) {
            IMMUTABLE
        } else {
            NO_CACHE
        }
    }

    /// The content type of its extension, in UTF-8 for text, `application/octet-stream` when none is known.
    fn content_type(&self) -> HeaderValue {
        let Some(mime) = mime_guess::from_path(&self.path).first() else {
            return HeaderValue::from_static("application/octet-stream");
        };

        let content_type =
            if mime.type_() == mime_guess::mime::TEXT && mime.get_param("charset").is_none() {
                format!("{mime}; charset=utf-8")
            } else {
                mime.to_string()
            };

        HeaderValue::from_str(&content_type)
            .unwrap_or_else(|_| HeaderValue::from_static("application/octet-stream"))
    }
}

/// The first second an HTTP date cannot hold, in the year 10000.
const YEAR_10000: u64 = 253_402_300_800;

/// `time` as an HTTP date, or `None` before 1970 or from the year 10000 on, which it cannot hold.
fn http_date(time: SystemTime) -> Option<HeaderValue> {
    let since = time.duration_since(UNIX_EPOCH).ok()?;

    if since.as_secs() >= YEAR_10000 {
        return None;
    }

    HeaderValue::from_str(&httpdate::fmt_http_date(time)).ok()
}

/// Whether the copy a request of `headers` holds is the file of `tag`, modified at `modified`: by `If-None-Match`,
/// or by `If-Modified-Since` when the request has no `If-None-Match`, as RFC 9110 says.
fn not_modified(headers: &HeaderMap, tag: &HeaderValue, modified: Option<SystemTime>) -> bool {
    let mut tags = headers.get_all(header::IF_NONE_MATCH).iter().peekable();

    if tags.peek().is_some() {
        let ours = weak(tag.to_str().unwrap_or_default());

        return tags
            .filter_map(|value| value.to_str().ok())
            .flat_map(|value| value.split(','))
            .map(str::trim)
            .any(|theirs| theirs == "*" || weak(theirs) == ours);
    }

    let since = headers
        .get(header::IF_MODIFIED_SINCE)
        .and_then(|since| since.to_str().ok())
        .and_then(|since| httpdate::parse_http_date(since).ok());
    let (Some(since), Some(modified)) = (since, modified) else {
        return false;
    };

    // An HTTP date counts whole seconds.
    let seconds = |time: SystemTime| {
        time.duration_since(UNIX_EPOCH)
            .map_or(0, |since| since.as_secs())
    };

    seconds(modified) <= seconds(since)
}

/// A tag without its weak mark, `W/`, which `If-None-Match` compares without.
fn weak(tag: &str) -> &str {
    tag.strip_prefix("W/").unwrap_or(tag)
}

/// Whether the file at `path` is named by its hash, as Vite names what it writes in `assets/`: `index-BXa3kD9f.js`, a
/// `-`, eight characters of base64url and a `.`. The eight hold a digit, which a word does not, so that a name written
/// by hand, `assets/logo-Darkmode.svg`, is not kept a year; a hash with no digit, about a quarter of them, is checked
/// again as any other file.
fn is_hashed(path: &str) -> bool {
    let Some(name) = path
        .strip_prefix(ASSETS)
        .and_then(|rest| rest.strip_prefix('/'))
    else {
        return false;
    };
    let name = name.rsplit('/').next().unwrap_or(name).as_bytes();

    name.iter().enumerate().any(|(at, &byte)| {
        let Some(hash) = name.get(at + 1..at + 9) else {
            return false;
        };

        byte == b'-'
            && name.get(at + 9) == Some(&b'.')
            && hash
                .iter()
                .all(|&byte| byte.is_ascii_alphanumeric() || byte == b'-' || byte == b'_')
            && hash.iter().any(u8::is_ascii_digit)
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_name_vite_writes_is_hashed() {
        assert!(is_hashed("assets/index-BXa3kD9f.js"));
        assert!(is_hashed("assets/index-B-3fk9aZ.css"));
        assert!(is_hashed("assets/vendor.lib-abc_1234.js"));
        assert!(is_hashed("assets/index-BXa3kD9f.js.map"));
        assert!(is_hashed("assets/fonts/inter-x1y2z3w4.woff2"));
    }

    #[test]
    fn any_other_name_is_not() {
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
    fn a_segment_out_of_the_directory_is_refused() {
        assert_eq!(segments("/a/../b"), Err(Refused::Outside));
        assert_eq!(segments("/a/%2e%2e/b"), Err(Refused::Outside));
        assert_eq!(segments("/a%2F..%2Fb"), Err(Refused::Outside));
        assert_eq!(segments("/a\\b"), Err(Refused::Unnamable));
        assert_eq!(segments("/a%00b"), Err(Refused::Unnamable));
        assert_eq!(
            segments("/assets//./app.js"),
            Ok(vec!["assets".to_owned(), "app.js".to_owned()])
        );
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
    fn a_path_with_an_extension_or_under_assets_is_no_route() {
        let route = |path: &str| is_route(&segments(path).expect("a path of the page"));

        assert!(route("/"));
        assert!(route("/notes/3"));
        assert!(!route("/assets/index-BXa3kD9f.js"));
        assert!(!route("/assets/gone"));
        assert!(!route("/favicon.ico"));
        assert!(!route("/notes/v1.2"));
    }

    #[test]
    fn a_time_out_of_the_http_dates_has_none() {
        let year_10000 = UNIX_EPOCH + std::time::Duration::from_secs(YEAR_10000);

        assert!(http_date(UNIX_EPOCH - std::time::Duration::from_secs(1)).is_none());
        assert!(http_date(year_10000).is_none());
        assert_eq!(
            http_date(year_10000 - std::time::Duration::from_secs(1)),
            Some(HeaderValue::from_static("Fri, 31 Dec 9999 23:59:59 GMT"))
        );
        assert_eq!(
            http_date(UNIX_EPOCH),
            Some(HeaderValue::from_static("Thu, 01 Jan 1970 00:00:00 GMT"))
        );
    }

    #[tokio::test]
    async fn a_file_that_grows_as_it_is_sent_is_cut_at_its_length() {
        let dir = tempfile::tempdir().expect("a temporary directory is made");
        let path = dir.path().join("log.txt");

        std::fs::write(&path, "first").expect("the file is written");

        let Opened::Yes(served) = find(dir.path(), &["log.txt".to_owned()], &[]) else {
            panic!("the file is found");
        };
        let response = served.respond(&HeaderMap::new(), false);

        std::fs::OpenOptions::new()
            .append(true)
            .open(&path)
            .and_then(|mut file| std::io::Write::write_all(&mut file, b" and more"))
            .expect("the file grows");

        let body = axum::body::to_bytes(response.into_body(), usize::MAX)
            .await
            .expect("the body is read");

        assert_eq!(body, "first");
    }

    #[cfg(unix)]
    #[test]
    fn a_file_opened_elsewhere_than_its_checked_path_is_refused() {
        let outer = tempfile::tempdir().expect("a temporary directory is made");
        let root = outer.path().join("dist");

        std::fs::create_dir(&root).expect("the directory is made");
        std::fs::write(root.join("app.js"), "inside").expect("the file is written");
        std::fs::write(outer.path().join("secret.txt"), "secret").expect("the file is written");

        let root = root.canonicalize().expect("a real path");
        let checked = root.join("app.js");
        let inside = File::open(&checked).expect("the file opens");
        let elsewhere = File::open(outer.path().join("secret.txt")).expect("the file opens");
        let metadata = |file: &File| file.metadata().expect("its metadata");

        assert!(is_still_under(&root, &checked, &inside, &metadata(&inside)));
        assert!(!is_still_under(
            &root,
            &checked,
            &elsewhere,
            &metadata(&elsewhere)
        ));
    }

    #[test]
    fn the_codings_come_by_their_weight() {
        let headers = |value: &'static str| {
            let mut headers = HeaderMap::new();

            headers.insert(header::ACCEPT_ENCODING, HeaderValue::from_static(value));
            headers
        };

        assert_eq!(
            accepted(&headers("gzip, deflate, br")),
            [Encoding::Brotli, Encoding::Gzip]
        );
        assert_eq!(
            accepted(&headers("br;q=0.5, gzip")),
            [Encoding::Gzip, Encoding::Brotli]
        );
        assert_eq!(accepted(&headers("br;q=0, gzip")), [Encoding::Gzip]);
        assert_eq!(accepted(&headers("*")), [Encoding::Brotli, Encoding::Gzip]);
        assert_eq!(accepted(&headers("identity")), []);
        assert_eq!(accepted(&HeaderMap::new()), []);
    }
}
