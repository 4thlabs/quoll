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

    let Some(segments) = segments(parts.uri.path()) else {
        return outside();
    };
    let encodings = accepted(&parts.headers);
    let found = tokio::task::spawn_blocking(move || find(&dir, &segments, &encodings)).await;

    match found {
        Ok(Opened::Yes(served)) => served.respond(&parts.headers, parts.method == Method::HEAD),
        Ok(Opened::Outside) => outside(),
        Ok(Opened::No) => {
            Error::not_found("the page has no file of this path, nor an `index.html`")
                .into_response()
        }
        Err(error) => Error::internal(error).into_response(),
    }
}

/// The 404 of a path that leads out of the page's directory.
fn outside() -> Response {
    Error::not_found("the path leads out of the page's directory").into_response()
}

/// The segments of a request's `path`, decoded: `None` when one leads out of the directory, `..`, or holds what no
/// segment of a file's path holds.
fn segments(path: &str) -> Option<Vec<String>> {
    let decoded = percent_decode_str(path).decode_utf8().ok()?;
    let mut segments = Vec::new();

    for segment in decoded.split('/') {
        match segment {
            "" | "." => {}
            ".." => return None,
            _ if segment.contains(['\\', '\0']) => return None,
            _ => segments.push(segment.to_owned()),
        }
    }

    Some(segments)
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

/// The file at `segments` under `dir`, or its `index.html` when there is none, opened in the first of `encodings`
/// stored beside it. Each is opened by its real path, its symbolic links followed, which must be in `dir`'s.
fn find(dir: &Path, segments: &[String], encodings: &[Encoding]) -> Opened<Served> {
    let Ok(root) = dir.canonicalize() else {
        return Opened::No;
    };

    let path = if segments.is_empty() {
        INDEX.to_owned()
    } else {
        segments.join("/")
    };

    match served(&root, &path, encodings) {
        Opened::No => served(&root, INDEX, encodings),
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

    match file.metadata() {
        Ok(metadata) if metadata.is_file() => Opened::Yes((file, metadata)),
        _ => Opened::No,
    }
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
        let modified = self.metadata.modified().ok();
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

        if let Some(modified) = modified {
            let date = httpdate::fmt_http_date(modified);

            if let Ok(date) = HeaderValue::from_str(&date) {
                answered.insert(header::LAST_MODIFIED, date);
            }
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

        if !head {
            let file = tokio::fs::File::from_std(self.file);

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
/// `-`, eight characters of base64url and a `.`. A hash of eight lowercase letters, which a word is too, is not taken
/// for one, so that `assets/my-elements.js` is not kept a year.
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
            && hash.iter().any(|byte| !byte.is_ascii_lowercase())
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
        assert!(!is_hashed("assets/index-BXa3kD9.js"));
        assert!(!is_hashed("assets/index-BXa3kD9fg.js"));
        assert!(!is_hashed("assetsx/index-BXa3kD9f.js"));
        assert!(!is_hashed("assets/é-BXa3kD9f"));
    }

    #[test]
    fn a_segment_out_of_the_directory_is_refused() {
        assert_eq!(segments("/a/../b"), None);
        assert_eq!(segments("/a/%2e%2e/b"), None);
        assert_eq!(segments("/a%2F..%2Fb"), None);
        assert_eq!(segments("/a\\b"), None);
        assert_eq!(
            segments("/assets//./app.js"),
            Some(vec!["assets".to_owned(), "app.js".to_owned()])
        );
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
