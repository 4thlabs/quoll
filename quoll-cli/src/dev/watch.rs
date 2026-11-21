//! What `cargo quoll dev` watches: the directories of the packages its apis and servers are built from, of the path
//! dependencies they have outside the workspace, and the workspace's `Cargo.toml` and `Cargo.lock`. One rule says
//! what a change counts as.

use std::collections::BTreeSet;
use std::fs;
use std::path::{Component, Path, PathBuf};
use std::time::Duration;

use anyhow::{Context, Result};
use notify_debouncer_full::notify::event::{AccessKind, AccessMode, MetadataKind, ModifyKind};
use notify_debouncer_full::notify::{self, EventKind, RecommendedWatcher, RecursiveMode};
use notify_debouncer_full::{
    DebounceEventResult, DebouncedEvent, Debouncer, RecommendedCache, new_debouncer,
};
use tokio::sync::mpsc::UnboundedSender;

use crate::cargo::Workspace;
use crate::dev::events::Reporter;

/// How long the events of a file are gathered: an editor's save, a write then a rename, is one change.
const DEBOUNCE: Duration = Duration::from_millis(200);

/// What to do when inotify has no watch left, which a big `node_modules` in a watched package uses up.
const LIMIT: &str = "inotify has no watch left: raise `fs.inotify.max_user_watches`, or keep the web side out of a \
                     package at the workspace's root";

/// The packages of the workspace and how they depend on each other, by their index in `Workspace::packages`.
#[derive(Debug, Clone)]
pub(crate) struct Graph {
    names: Vec<String>,
    /// The canonical directory of each package, as the paths of the watcher are matched in their canonical form.
    dirs: Vec<PathBuf>,
    /// The packages of the workspace each one is built with, directly.
    dependencies: Vec<Vec<usize>>,
    /// The path dependencies outside the workspace's packages, by their canonical directory, with the package that
    /// depends on them.
    external: Vec<(PathBuf, usize)>,
}

/// What the watcher sends: the packages whose files changed, or its error.
#[derive(Debug)]
pub(crate) enum Watched {
    Changed(BTreeSet<usize>),
    Error(String),
}

/// The watcher, which watches until it is dropped.
pub(crate) struct Watcher {
    _debouncer: Debouncer<RecommendedWatcher, RecommendedCache>,
}

impl Graph {
    /// Reads the packages of `workspace`, their directories made canonical, which reads the file system.
    pub(crate) fn new(workspace: &Workspace) -> Self {
        let names = workspace
            .packages
            .iter()
            .map(|package| package.name.clone())
            .collect();
        let dirs: Vec<_> = workspace
            .packages
            .iter()
            .map(|package| canonical(package.dir()))
            .collect();
        let mut dependencies = Vec::new();
        let mut external = Vec::new();

        for (index, package) in workspace.packages.iter().enumerate() {
            let mut members = Vec::new();

            for path in package.path_dependencies() {
                let path = canonical(path);

                match dirs.iter().position(|dir| *dir == path) {
                    Some(member) => members.push(member),
                    None => external.push((path, index)),
                }
            }

            dependencies.push(members);
        }

        Self {
            names,
            dirs,
            dependencies,
            external,
        }
    }

    /// The index of the package named `name`.
    pub(crate) fn index(&self, name: &str) -> usize {
        self.names
            .iter()
            .position(|candidate| candidate == name)
            .expect("the config names packages of the workspace only")
    }

    pub(crate) fn name(&self, index: usize) -> &str {
        &self.names[index]
    }

    /// Whether `package` is `changed`, or is built with it, through the packages of the workspace.
    pub(crate) fn depends_on(&self, package: usize, changed: usize) -> bool {
        self.closure([package]).contains(&changed)
    }

    /// `packages`, and the packages of the workspace they are built with.
    pub(crate) fn closure(&self, packages: impl IntoIterator<Item = usize>) -> BTreeSet<usize> {
        let mut found = BTreeSet::new();
        let mut next: Vec<usize> = packages.into_iter().collect();

        while let Some(package) = next.pop() {
            if found.insert(package) {
                next.extend(&self.dependencies[package]);
            }
        }

        found
    }

    /// The package whose directory holds the canonical `path`, the innermost when packages are nested, with the
    /// path from it.
    fn package_of<'a>(&self, path: &'a Path) -> Option<(usize, &'a Path)> {
        self.dirs
            .iter()
            .enumerate()
            .filter_map(|(index, dir)| Some((index, dir, path.strip_prefix(dir).ok()?)))
            .max_by_key(|(_, dir, _)| dir.components().count())
            .map(|(index, _, relative)| (index, relative))
    }
}

/// What is watched, and what a change counts as: the rule of both.
#[derive(Debug)]
struct Rule {
    graph: Graph,
    watched: BTreeSet<usize>,
    /// The target directory, canonical, which cargo writes.
    target: PathBuf,
    /// `web.dir`, canonical, whose bundler watches its own files.
    web: Option<PathBuf>,
    /// The documents, canonical, which `cargo quoll dev` writes.
    outputs: Vec<PathBuf>,
    /// The workspace's `Cargo.toml` and `Cargo.lock`, canonical, whose change counts for every package.
    manifests: Vec<PathBuf>,
}

impl Rule {
    /// The watched package a canonical `path` counts for, a file or a directory: none when it is left out.
    ///
    /// Left out are what is in no watched package, the package's own `target/`, a hidden file or directory, or
    /// `node_modules`, at any depth, the target directory, the documents, and what is in `web.dir`, unless the
    /// package it is in is in `web.dir` too, as a Tauri app's `src-tauri`.
    fn package(&self, path: &Path) -> Option<usize> {
        let (package, relative) = self.graph.package_of(path)?;

        if !self.watched.contains(&package) {
            return None;
        }

        if !self.counts(path, relative) {
            return None;
        }

        if let Some(web) = &self.web
            && path.starts_with(web)
            && !self.graph.dirs[package].starts_with(web)
        {
            return None;
        }

        Some(package)
    }

    /// The watched packages a change of the canonical `path` counts for: the package it is in, the packages that
    /// depend on the path dependency it is in, or all of them for the workspace's `Cargo.toml` and `Cargo.lock`.
    fn packages(&self, path: &Path) -> BTreeSet<usize> {
        if self.manifests.iter().any(|manifest| manifest == path) {
            return self.watched.clone();
        }

        let mut packages: BTreeSet<_> = self.package(path).into_iter().collect();

        for (dir, dependent) in &self.graph.external {
            let Ok(relative) = path.strip_prefix(dir) else {
                continue;
            };

            if self.watched.contains(dependent) && self.counts(path, relative) {
                packages.insert(*dependent);
            }
        }

        packages
    }

    /// Whether `path`, `relative` from the directory of its package, is left in: not in its own `target/`, in nothing
    /// hidden nor `node_modules`, not in the target directory, and no document.
    fn counts(&self, path: &Path, relative: &Path) -> bool {
        let first = relative.components().next();

        if first.is_some_and(|first| first.as_os_str() == "target")
            || relative.components().any(hidden_or_modules)
        {
            return false;
        }

        !path.starts_with(&self.target) && !self.outputs.iter().any(|output| output == path)
    }

    /// The packages `events` change: every watched one when the watcher missed some, and asks for a rescan.
    fn changed(&self, events: &[DebouncedEvent]) -> BTreeSet<usize> {
        let mut changed = BTreeSet::new();

        for event in events {
            if event.need_rescan() {
                return self.watched.clone();
            }

            if !changes(&event.kind) {
                continue;
            }

            for path in &event.paths {
                changed.extend(self.packages(&canonical(path)));
            }
        }

        changed
    }

    /// The directories to watch, each with its tree but the workspace's root, of which the manifests alone count. A
    /// directory inside another is watched with it.
    fn dirs(&self, root: &Path) -> Vec<(PathBuf, RecursiveMode)> {
        let trees: Vec<&PathBuf> = self
            .watched
            .iter()
            .map(|&package| &self.graph.dirs[package])
            .chain(
                self.graph
                    .external
                    .iter()
                    .filter(|(_, dependent)| self.watched.contains(dependent))
                    .map(|(dir, _)| dir),
            )
            .collect();
        let mut dirs: Vec<_> = trees
            .iter()
            .filter(|dir| {
                !trees
                    .iter()
                    .any(|other| other != *dir && dir.starts_with(other))
            })
            .map(|dir| ((*dir).clone(), RecursiveMode::Recursive))
            .collect();

        dirs.sort();
        dirs.dedup();

        if !trees.iter().any(|dir| root.starts_with(dir)) {
            dirs.push((root.to_path_buf(), RecursiveMode::NonRecursive));
        }

        dirs
    }
}

/// Whether a part of a path inside a package leaves it out: a hidden file or directory, as `.git` or an editor's swap
/// file, or `node_modules`.
fn hidden_or_modules(component: Component<'_>) -> bool {
    let Component::Normal(name) = component else {
        return false;
    };

    name == "node_modules" || name.to_string_lossy().starts_with('.')
}

/// Whether an event changes a file: a read does not, as cargo's own build of the file would start another one.
fn changes(kind: &EventKind) -> bool {
    match kind {
        EventKind::Access(AccessKind::Close(AccessMode::Write)) => true,
        EventKind::Access(_)
        | EventKind::Other
        | EventKind::Modify(ModifyKind::Metadata(MetadataKind::AccessTime)) => false,
        EventKind::Any | EventKind::Create(_) | EventKind::Modify(_) | EventKind::Remove(_) => true,
    }
}

/// Watches the directories of `packages`, of those they are built with and of their path dependencies, and sends
/// the packages whose files change. It reads the file system: it is called before the runtime starts. It returns the
/// watcher and how many packages it watches.
pub(crate) fn start(
    workspace: &Workspace,
    graph: Graph,
    packages: impl IntoIterator<Item = usize>,
    sender: UnboundedSender<Watched>,
    reporter: &Reporter,
) -> Result<(Watcher, usize)> {
    let watched = graph.closure(packages);
    let config = &workspace.config;
    let root = canonical(&workspace.root);
    let rule = Rule {
        watched,
        target: canonical(&workspace.target_directory),
        web: config
            .web
            .as_ref()
            .map(|web| canonical(&workspace.root.join(&web.dir))),
        outputs: config
            .apis
            .iter()
            .map(|api| canonical(&workspace.root.join(&api.output)))
            .collect(),
        manifests: vec![root.join("Cargo.toml"), root.join("Cargo.lock")],
        graph,
    };
    let dirs = rule.dirs(&root);
    let count = rule.watched.len();

    let mut debouncer = new_debouncer(DEBOUNCE, None, move |result: DebounceEventResult| {
        let watched = match result {
            Ok(events) => {
                let changed = rule.changed(&events);

                if changed.is_empty() {
                    return;
                }

                Watched::Changed(changed)
            }
            Err(errors) => Watched::Error(errors.iter().map(said).collect::<Vec<_>>().join(", ")),
        };

        // `cargo quoll dev` is stopping when no one receives.
        let _ = sender.send(watched);
    })
    .context("cannot watch the workspace")?;

    for (dir, mode) in dirs {
        match debouncer.watch(&dir, mode) {
            Ok(()) => {}
            // What is watched still is: the rest of the files are not seen, which the warning says.
            Err(error) if matches!(error.kind, notify::ErrorKind::MaxFilesWatch) => {
                reporter.status(
                    "Warning",
                    format!("cannot watch all of {}: {LIMIT}", dir.display()),
                );
            }
            Err(error) => {
                return Err(error).with_context(|| format!("cannot watch {}", dir.display()));
            }
        }
    }

    Ok((
        Watcher {
            _debouncer: debouncer,
        },
        count,
    ))
}

/// An error of the watcher, which names inotify's limit when it is the one reached.
fn said(error: &notify::Error) -> String {
    if matches!(error.kind, notify::ErrorKind::MaxFilesWatch) {
        return LIMIT.to_owned();
    }

    error.to_string()
}

/// The canonical form of `path`, as a watcher gives it on macOS, `/private/var` for `/var`. A path that does not exist
/// yet, a document not yet written or a file removed, is its nearest existing directory's, made canonical, then the
/// rest of it.
fn canonical(path: &Path) -> PathBuf {
    let mut existing = path;
    let mut rest = Vec::new();

    loop {
        if let Ok(canonical) = fs::canonicalize(existing) {
            return rest
                .iter()
                .rev()
                .fold(canonical, |path, part| path.join(part));
        }

        let (Some(parent), Some(name)) = (existing.parent(), existing.file_name()) else {
            return path.to_path_buf();
        };

        rest.push(name);
        existing = parent;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::cargo::Package;

    fn graph() -> Graph {
        let names = ["models", "api", "server", "tools", "root", "app"];

        Graph {
            names: names.iter().map(|name| (*name).to_owned()).collect(),
            dirs: [
                "/ws/models",
                "/ws/api",
                "/ws/server",
                "/ws/tools",
                "/ws",
                "/ws/web/src-tauri",
            ]
            .iter()
            .map(PathBuf::from)
            .collect(),
            dependencies: vec![vec![], vec![0], vec![1], vec![], vec![], vec![]],
            external: vec![(PathBuf::from("/shared/auth"), 1)],
        }
    }

    fn rule(web: &str) -> Rule {
        Rule {
            graph: graph(),
            watched: BTreeSet::from([0, 1, 2, 4, 5]),
            target: PathBuf::from("/ws/target"),
            web: Some(PathBuf::from(web)),
            outputs: vec![PathBuf::from("/ws/api/openapi.json")],
            manifests: vec![
                PathBuf::from("/ws/Cargo.toml"),
                PathBuf::from("/ws/Cargo.lock"),
            ],
        }
    }

    #[test]
    fn a_read_is_no_change() {
        use notify_debouncer_full::notify::event::RemoveKind;

        assert!(!changes(&EventKind::Access(AccessKind::Open(
            AccessMode::Read
        ))));
        assert!(!changes(&EventKind::Access(AccessKind::Close(
            AccessMode::Read
        ))));
        assert!(!changes(&EventKind::Modify(ModifyKind::Metadata(
            MetadataKind::AccessTime
        ))));
        assert!(changes(&EventKind::Access(AccessKind::Close(
            AccessMode::Write
        ))));
        assert!(changes(&EventKind::Modify(ModifyKind::Any)));
        assert!(changes(&EventKind::Remove(RemoveKind::File)));
    }

    #[test]
    fn a_server_depends_on_the_crates_of_its_contract() {
        let graph = graph();

        assert!(graph.depends_on(2, 2));
        assert!(graph.depends_on(2, 1));
        assert!(graph.depends_on(2, 0));
        assert!(!graph.depends_on(1, 2));
        assert!(!graph.depends_on(2, 3));
        assert_eq!(graph.closure([2]), BTreeSet::from([0, 1, 2]));
    }

    #[test]
    fn a_dev_dependency_is_not_built_with_the_binary() {
        let package: Package = serde_json::from_value(serde_json::json!({
            "name": "server",
            "id": "path+file:///ws/server#server@0.1.0",
            "manifest_path": "/ws/server/Cargo.toml",
            "targets": [],
            "dependencies": [
                { "name": "api", "kind": null, "path": "/ws/api" },
                { "name": "build-tools", "kind": "build", "path": "/ws/build-tools" },
                { "name": "test-tools", "kind": "dev", "path": "/ws/tools" },
                { "name": "serde", "kind": null },
            ],
        }))
        .expect("the package is one of cargo metadata");

        let paths: Vec<_> = package.path_dependencies().collect();

        assert_eq!(paths, [Path::new("/ws/api"), Path::new("/ws/build-tools")]);
    }

    #[test]
    fn a_path_belongs_to_the_innermost_package() {
        let graph = graph();

        assert_eq!(
            graph.package_of(Path::new("/ws/server/src/main.rs")),
            Some((2, Path::new("src/main.rs")))
        );
        assert_eq!(
            graph.package_of(Path::new("/ws/README.md")),
            Some((4, Path::new("README.md")))
        );
        assert_eq!(graph.package_of(Path::new("/elsewhere/main.rs")), None);
    }

    #[test]
    fn what_cargo_quoll_writes_and_hidden_files_do_not_count() {
        let rule = rule("/ws/web");
        let package = |path: &str| rule.package(Path::new(path));

        assert_eq!(package("/ws/api/src/lib.rs"), Some(1));
        assert_eq!(package("/ws/api/openapi.json"), None);
        assert_eq!(package("/ws/api/.openapi.json.12.tmp"), None);
        assert_eq!(package("/ws/server/src/.main.rs.swp"), None);
        assert_eq!(package("/ws/server/.git/HEAD"), None);
        assert_eq!(package("/ws/server/target/debug/server"), None);
        assert_eq!(package("/ws/target/debug/server"), None);
        assert_eq!(package("/ws/tools/src/main.rs"), None, "not watched");
        assert_eq!(package("/elsewhere/main.rs"), None);
    }

    #[test]
    fn only_the_package_s_own_target_is_left_out() {
        let rule = rule("/ws/web");

        assert_eq!(
            rule.package(Path::new("/ws/server/src/target/mod.rs")),
            Some(2)
        );
        assert_eq!(rule.package(Path::new("/ws/server/target")), None);
    }

    #[test]
    fn node_modules_is_left_out_at_any_depth() {
        let rule = rule("/ws/web");

        assert_eq!(rule.package(Path::new("/ws/node_modules/x/index.js")), None);
        assert_eq!(
            rule.package(Path::new("/ws/server/assets/node_modules/x")),
            None
        );
        assert_eq!(
            rule.package(Path::new("/ws/server/assets/logo.svg")),
            Some(2)
        );
    }

    #[test]
    fn the_web_side_is_left_out_but_the_packages_in_it() {
        let rule = rule("/ws/web");

        assert_eq!(rule.package(Path::new("/ws/web/src/main.ts")), None);
        assert_eq!(
            rule.package(Path::new("/ws/web/src-tauri/src/main.rs")),
            Some(5)
        );

        // A web side at the workspace's root holds every package, the one at the root too.
        let rule = self::rule("/ws");

        assert_eq!(rule.package(Path::new("/ws/server/src/main.rs")), Some(2));
        assert_eq!(rule.package(Path::new("/ws/web/src/main.ts")), Some(4));
    }

    #[test]
    fn a_path_dependency_outside_the_workspace_counts_for_its_dependent() {
        let rule = rule("/ws/web");

        assert_eq!(
            rule.packages(Path::new("/shared/auth/src/lib.rs")),
            BTreeSet::from([1])
        );
        assert_eq!(
            rule.packages(Path::new("/shared/auth/target/debug/x")),
            BTreeSet::new()
        );
        assert_eq!(
            rule.packages(Path::new("/shared/other/src/lib.rs")),
            BTreeSet::new()
        );
    }

    #[test]
    fn the_workspace_s_manifests_count_for_every_package() {
        let rule = rule("/ws/web");

        assert_eq!(rule.packages(Path::new("/ws/Cargo.lock")), rule.watched);
        assert_eq!(rule.packages(Path::new("/ws/Cargo.toml")), rule.watched);
    }

    #[test]
    fn a_rescan_counts_for_every_package() {
        use notify_debouncer_full::notify::event::Flag;
        use std::time::Instant;

        let rule = rule("/ws/web");
        let event = |event: notify::Event| DebouncedEvent::new(event, Instant::now());

        let read = event(
            notify::Event::new(EventKind::Access(AccessKind::Open(AccessMode::Read)))
                .add_path(PathBuf::from("/ws/api/src/lib.rs")),
        );
        assert_eq!(rule.changed(std::slice::from_ref(&read)), BTreeSet::new());

        let rescan = event(notify::Event::new(EventKind::Other).set_flag(Flag::Rescan));
        assert_eq!(rule.changed(&[read, rescan]), rule.watched);
    }

    #[test]
    fn a_directory_inside_another_is_watched_with_it() {
        let rule = rule("/ws/web");
        let dirs = rule.dirs(Path::new("/ws"));

        assert_eq!(
            dirs.iter()
                .map(|(dir, _)| dir.as_path())
                .collect::<Vec<_>>(),
            [Path::new("/shared/auth"), Path::new("/ws")],
            "the root package holds the others"
        );

        let mut rule = rule;

        rule.watched = BTreeSet::from([1, 2]);

        let dirs = rule.dirs(Path::new("/ws"));
        let watched: Vec<_> = dirs
            .iter()
            .map(|(dir, mode)| (dir.as_path(), *mode == RecursiveMode::Recursive))
            .collect();

        assert_eq!(
            watched,
            [
                (Path::new("/shared/auth"), true),
                (Path::new("/ws/api"), true),
                (Path::new("/ws/server"), true),
                (Path::new("/ws"), false),
            ]
        );
    }

    #[cfg(unix)]
    #[test]
    fn a_path_not_yet_written_is_canonical_through_its_directory() {
        let dir = tempfile::tempdir().expect("a temporary directory");
        let real = canonical(dir.path()).join("real");
        let link = dir.path().join("link");

        fs::create_dir(&real).expect("the directory is made");
        std::os::unix::fs::symlink(&real, &link).expect("the link is made");

        assert_eq!(
            canonical(&link.join("web/openapi.json")),
            real.join("web/openapi.json")
        );
        assert_eq!(canonical(&link), real);
    }
}
