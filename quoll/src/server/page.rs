//! The page a bundler builds, served from its directory beside the api: each file of the directory, and `index.html`
//! for a request of a page that is no file, so that the page's router takes it.

use std::collections::HashSet;
use std::convert::Infallible;
use std::error::Error as StdError;
use std::fmt;
use std::fs::{File, Metadata};
use std::future::Future;
use std::io;
use std::path::Component;
use std::path::{Path, PathBuf};
use std::pin::Pin;
use std::sync::{Arc, Mutex, PoisonError};
use std::task::{Context, Poll};
use std::time::{SystemTime, UNIX_EPOCH};

use axum::body::Body;
use axum::extract::Request;
use axum::response::{IntoResponse, Response};
use http::{HeaderMap, HeaderValue, Method, StatusCode, header};
use percent_encoding::{AsciiSet, CONTROLS, percent_decode_str, utf8_percent_encode};
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

/// The manifest Vite writes with `build.manifest`, which names the files it hashed, under the page's directory.
const MANIFEST: &str = ".vite/manifest.json";

/// The files of a page's directory, a service an axum router falls back to.
#[derive(Debug, Clone)]
pub(crate) struct Page {
    dir: Arc<Path>,
    manifest: Arc<Manifest>,
}

impl Page {
    /// The page of the files in `dir`, read at each request.
    pub(crate) fn new(dir: PathBuf) -> Self {
        Self {
            dir: dir.into(),
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

        Box::pin(async move { Ok(answer(page, request).await) })
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

/// The answer to `request` from the files of `page`.
async fn answer(page: Page, request: Request) -> Response {
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
    let wanted = Wanted {
        slashed: parts.uri.path().ends_with('/'),
        query: parts.uri.query().map(str::to_owned),
        // Never under `/.well-known/`, whose files are read by programs.
        page: accepts_html(&parts.headers)
            && segments.first().map(String::as_str) != Some(WELL_KNOWN),
        encodings: accepted(&parts.headers),
    };
    let found = tokio::task::spawn_blocking(move || page.find(&segments, &wanted)).await;

    match found {
        Ok(Found::File(served)) => served.respond(&parts.headers, parts.method == Method::HEAD),
        Ok(Found::Directory(location)) => {
            let mut response = Response::new(Body::empty());

            *response.status_mut() = StatusCode::PERMANENT_REDIRECT;
            response.headers_mut().insert(header::LOCATION, location);
            response
        }
        Ok(Found::Refused(refused)) => Error::not_found(refused.detail()).into_response(),
        Ok(Found::Missing) => Error::not_found("the page has no file of this path").into_response(),
        Err(error) => Error::internal(error).into_response(),
    }
}

/// What a request wants of the page, besides its path.
struct Wanted {
    /// Whether its path ends with a `/`, as a directory's does.
    slashed: bool,
    /// Its query, which a redirection keeps.
    query: Option<String>,
    /// Whether it asks for a page, which `index.html` answers when its path is no file.
    page: bool,
    /// The codings it accepts, the most wanted first.
    encodings: Vec<Encoding>,
}

/// What a path of the page leads to.
enum Found {
    /// A file, boxed as it is the largest by far.
    File(Box<Served>),
    /// A directory with an `index.html`, asked without its final `/`: the `Location` of the path with it.
    Directory(HeaderValue),
    /// A file the page does not serve.
    Refused(Refused),
    /// No file.
    Missing,
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

/// What a segment of a path holds escaped, as the URL standard escapes a path segment, and its `/` and `%`.
const SEGMENT: &AsciiSet = &CONTROLS
    .add(b' ')
    .add(b'"')
    .add(b'#')
    .add(b'<')
    .add(b'>')
    .add(b'?')
    .add(b'`')
    .add(b'{')
    .add(b'}')
    .add(b'/')
    .add(b'%');

/// The path of `segments` with a final `/`, and `query`: the `Location` of a directory asked without it. It is
/// written from the segments, so it always begins with one `/` and a segment, never `//`, another host.
fn directory(segments: &[String], query: Option<&str>) -> Option<HeaderValue> {
    let mut location = String::new();

    for segment in segments {
        location.push('/');
        location.extend(utf8_percent_encode(segment, SEGMENT));
    }

    location.push('/');

    if let Some(query) = query {
        location.push('?');
        location.push_str(query);
    }

    HeaderValue::from_str(&location).ok()
}

/// Whether `headers` accept `text/html`, as a browser's request of a page does, and a script's or an image's does
/// not: the request that `index.html` answers when its path is no file.
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

/// The weight of an item of `Accept` or `Accept-Encoding`, from its parameters: its `q`, 1 when it has none, 0 when
/// it does not read.
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
        let weight = weight(parts);

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

impl Page {
    /// The file at `segments` under the page's directory: the file of that path, else the `index.html` of a
    /// directory of that path, else the page's `index.html` when the request asks for a page. Each is opened by its
    /// real path, its symbolic links followed, which must be in the directory's, in the first coding of the request's
    /// stored beside it.
    fn find(&self, segments: &[String], wanted: &Wanted) -> Found {
        let Ok(root) = self.dir.canonicalize() else {
            return Found::Missing;
        };
        let hashed = self.manifest.hashed(&root);
        let served = |path: &str| served(&root, path, &wanted.encodings, &hashed);

        if segments.is_empty() {
            return served(INDEX).into();
        }

        let path = segments.join("/");

        match served(&path) {
            Opened::No => {}
            opened => return opened.into(),
        }

        // A file of that path is no directory, whose `index.html` is not opened then.
        match served(&format!("{path}/{INDEX}")) {
            Opened::Yes(_) if !wanted.slashed => {
                return directory(segments, wanted.query.as_deref())
                    .map_or(Found::Missing, Found::Directory);
            }
            Opened::No => {}
            opened => return opened.into(),
        }

        if wanted.page {
            return served(INDEX).into();
        }

        Found::Missing
    }
}

impl From<Opened<Served>> for Found {
    fn from(opened: Opened<Served>) -> Self {
        match opened {
            Opened::Yes(served) => Self::File(Box::new(served)),
            Opened::Refused(refused) => Self::Refused(refused),
            Opened::No => Self::Missing,
        }
    }
}

/// What opening a file gives.
enum Opened<T> {
    /// The file.
    Yes(T),
    /// A file the page does not serve: out of the directory, or hidden, by its real path.
    Refused(Refused),
    /// No file.
    No,
}

/// The file at `path` under `root`, in the first of `encodings` stored beside it, or as it is, kept a year when
/// `hashed` says the bundler named it by its hash.
fn served(root: &Path, path: &str, encodings: &[Encoding], hashed: &Hashed) -> Opened<Served> {
    let plain = root.join(path);
    let (file, metadata) = match open(root, &plain) {
        Opened::Yes(opened) => opened,
        Opened::Refused(refused) => return Opened::Refused(refused),
        Opened::No => return Opened::No,
    };
    let cache = if hashed.names(path) {
        IMMUTABLE
    } else {
        NO_CACHE
    };
    let served = |file, metadata, encoding| Served {
        path: path.to_owned(),
        file,
        metadata,
        encoding,
        cache: cache.clone(),
    };

    // A coding stored out of the directory, hidden, or that cannot be read, is passed over.
    for &encoding in encodings {
        let mut stored = plain.clone().into_os_string();

        stored.push(".");
        stored.push(encoding.extension());

        if let Opened::Yes((file, metadata)) = open(root, Path::new(&stored)) {
            return Opened::Yes(served(file, metadata, Some(encoding)));
        }
    }

    Opened::Yes(served(file, metadata, None))
}

/// The regular file at `path`, by its real path, which must be under `root`, itself a real path, and not hidden.
fn open(root: &Path, path: &Path) -> Opened<(File, Metadata)> {
    let Ok(real) = path.canonicalize() else {
        return Opened::No;
    };

    if let Err(refused) = under(root, &real) {
        return Opened::Refused(refused);
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

    match opened_under(root, &real, &file, &metadata) {
        Opened::Yes(()) => Opened::Yes((file, metadata)),
        Opened::Refused(refused) => Opened::Refused(refused),
        Opened::No => Opened::No,
    }
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

/// Whether `file`, opened at `real`, a real path the page serves under `root`, is still one: a directory of the path
/// swapped for a symbolic link between the check and the open would have opened a file elsewhere.
///
/// On Linux, the real path of the opened file is read through `/proc/self/fd`. Elsewhere, or without `/proc`, by
/// [`reopened_under`].
fn opened_under(root: &Path, real: &Path, file: &File, metadata: &Metadata) -> Opened<()> {
    #[cfg(target_os = "linux")]
    {
        use std::os::fd::AsRawFd;

        if let Ok(opened) = std::fs::read_link(format!("/proc/self/fd/{}", file.as_raw_fd())) {
            return match under(root, &opened) {
                Ok(()) => Opened::Yes(()),
                Err(refused) => Opened::Refused(refused),
            };
        }
    }

    #[cfg(not(target_os = "linux"))]
    let _ = file;

    reopened_under(root, real, metadata)
}

/// Whether `real` is still a real path the page serves under `root`, made real again, and on Unix still the file of
/// `metadata`, by its device and inode: a link swapped in and out again between the two checks is not seen. A file
/// replaced or removed meanwhile, as a deployment does, is no file, not one out of the directory.
fn reopened_under(root: &Path, real: &Path, metadata: &Metadata) -> Opened<()> {
    let Ok(again) = real.canonicalize() else {
        return Opened::No;
    };

    if let Err(refused) = under(root, &again) {
        return Opened::Refused(refused);
    }

    if !is_same_file(&again, metadata) {
        return Opened::No;
    }

    Opened::Yes(())
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

/// The names of the files Vite hashed, read from its manifest, again when the manifest changes.
#[derive(Debug, Default)]
struct Manifest {
    read: Mutex<Option<Read>>,
}

/// A manifest read: what it was when read, and the files it names, `None` when it does not read.
#[derive(Debug)]
struct Read {
    stamp: (Option<SystemTime>, u64),
    files: Option<Arc<HashSet<String>>>,
}

/// How the files named by their hash are told.
enum Hashed {
    /// By the manifest of Vite, which names them.
    Listed(Arc<HashSet<String>>),
    /// By their names alone, with no manifest: [`is_hashed`].
    Named,
}

impl Hashed {
    /// Whether the file at `path`, under the page's directory, is named by its hash.
    fn names(&self, path: &str) -> bool {
        match self {
            Self::Listed(files) => files.contains(path),
            Self::Named => is_hashed(path),
        }
    }
}

impl Manifest {
    /// How the files under `root`, a real path, named by their hash are told: by `root/.vite/manifest.json` when it
    /// reads, read again when its time or its length change, else by their names.
    fn hashed(&self, root: &Path) -> Hashed {
        let path = root.join(MANIFEST);
        let metadata = path
            .canonicalize()
            .ok()
            .filter(|real| real.starts_with(root))
            .and_then(|real| std::fs::metadata(real).ok())
            .filter(Metadata::is_file);
        let mut read = self.read.lock().unwrap_or_else(PoisonError::into_inner);

        let Some(metadata) = metadata else {
            *read = None;
            return Hashed::Named;
        };

        let stamp = (metadata.modified().ok(), metadata.len());

        if read.as_ref().is_none_or(|read| read.stamp != stamp) {
            let files = std::fs::read(&path)
                .ok()
                .and_then(|bytes| listed(&bytes))
                .map(Arc::new);

            if files.is_none() {
                tracing::warn!(
                    manifest = %path.display(),
                    "the page's Vite manifest does not read: its hashed files are told by their names"
                );
            }

            *read = Some(Read { stamp, files });
        }

        match read.as_ref().and_then(|read| read.files.clone()) {
            Some(files) => Hashed::Listed(files),
            None => Hashed::Named,
        }
    }
}

/// The files a manifest of Vite names, the `file`, `css` and `assets` of each of its chunks, or `None` when `bytes`
/// is not one.
fn listed(bytes: &[u8]) -> Option<HashSet<String>> {
    let manifest: serde_json::Map<String, serde_json::Value> =
        serde_json::from_slice(bytes).ok()?;
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

    Some(files)
}

/// A file of the page, opened, to be answered.
struct Served {
    /// Its path under the directory, `/` between its segments, which names its type.
    path: String,
    file: File,
    metadata: Metadata,
    /// The coding it is stored in, or `None` as it is.
    encoding: Option<Encoding>,
    /// Its `Cache-Control`: kept a year when the bundler named it by its hash, checked again otherwise.
    cache: HeaderValue,
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
        let tag = self.tag();
        let mut response = Response::new(Body::empty());
        let answered = response.headers_mut();

        answered.insert(header::ETAG, tag.clone());
        answered.insert(header::CACHE_CONTROL, self.cache.clone());
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

    /// Its entity tag: its length, the time it was modified, on Unix its inode and the time its inode changed, and
    /// its coding. A build that fixes the time of its files, `SOURCE_DATE_EPOCH`, writes each anew, with a new inode
    /// and change time. It is weak, as a layer may compress a file stored as it is, which keeps its tag.
    fn tag(&self) -> HeaderValue {
        let nanos = self
            .metadata
            .modified()
            .ok()
            .and_then(|modified| modified.duration_since(UNIX_EPOCH).ok())
            .map_or(0, |since| since.as_nanos());
        let coding = self.encoding.map_or(String::new(), |encoding| {
            format!("-{}", encoding.extension())
        });
        let tag = format!(
            "W/\"{:x}-{nanos:x}{}{coding}\"",
            self.metadata.len(),
            inode(&self.metadata)
        );

        HeaderValue::from_str(&tag).expect("a tag of hexadecimal digits is a header value")
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

/// The inode of `metadata` and the time it changed, for its tag.
#[cfg(unix)]
fn inode(metadata: &Metadata) -> String {
    use std::os::unix::fs::MetadataExt;

    format!(
        "-{:x}-{:x}.{:x}",
        metadata.ino(),
        metadata.ctime(),
        metadata.ctime_nsec()
    )
}

/// Windows names a file by no stable number.
#[cfg(not(unix))]
fn inode(_metadata: &Metadata) -> String {
    String::new()
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

/// Whether the file at `path` is named by its hash, with no manifest of Vite, as Vite names what it writes in `assets/`: `index-BXa3kD9f.js`, a
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
    fn a_directory_is_redirected_to_its_path_with_a_slash() {
        let segments = |path: &str| segments(path).expect("a path of the page");

        assert_eq!(
            directory(&segments("/docs"), Some("a=1&b")),
            Some(HeaderValue::from_static("/docs/?a=1&b"))
        );
        assert_eq!(
            directory(&segments("/r%C3%A9sum%C3%A9/a%20b"), None),
            Some(HeaderValue::from_static("/r%C3%A9sum%C3%A9/a%20b/"))
        );
        // Never `//`, which a browser reads as another host.
        assert_eq!(
            directory(&segments("//evil.example"), None),
            Some(HeaderValue::from_static("/evil.example/"))
        );
        assert_eq!(
            directory(&segments("/a%2F%2Fb"), None),
            Some(HeaderValue::from_static("/a/b/"))
        );
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
        assert_eq!(listed(b"[1, 2]"), None);
        assert_eq!(listed(b"not json"), None);
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

        let wanted = Wanted {
            slashed: false,
            query: None,
            page: false,
            encodings: Vec::new(),
        };
        let found = Page::new(dir.path().to_owned()).find(&["log.txt".to_owned()], &wanted);
        let Found::File(served) = found else {
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

        assert!(matches!(
            opened_under(&root, &checked, &inside, &metadata(&inside)),
            Opened::Yes(())
        ));
        assert!(!matches!(
            opened_under(&root, &checked, &elsewhere, &metadata(&elsewhere)),
            Opened::Yes(())
        ));
        assert!(matches!(
            reopened_under(&root, &checked, &metadata(&elsewhere)),
            Opened::No
        ));
    }

    #[cfg(unix)]
    #[test]
    fn a_file_replaced_or_removed_after_its_open_is_no_file_not_one_outside() {
        let dir = tempfile::tempdir().expect("a temporary directory is made");
        let root = dir.path().canonicalize().expect("a real path");
        let checked = root.join("app.js");

        std::fs::write(&checked, "old").expect("the file is written");

        let opened = File::open(&checked).expect("the file opens");
        let metadata = opened.metadata().expect("its metadata");

        // A deployment writes the new file beside it, then renames it over the old one.
        std::fs::write(root.join("app.js.new"), "new").expect("the file is written");
        std::fs::rename(root.join("app.js.new"), &checked).expect("the file is replaced");

        assert!(matches!(
            reopened_under(&root, &checked, &metadata),
            Opened::No
        ));

        std::fs::remove_file(&checked).expect("the file is removed");

        assert!(matches!(
            reopened_under(&root, &checked, &metadata),
            Opened::No
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
