//! What `cargo quoll dev` watches: the directories of the packages its apis and servers are built from, and the
//! package a changed file belongs to. One rule says both, so that what is watched and what counts never disagree.

use std::collections::BTreeSet;
use std::fs;
use std::path::{Component, Path, PathBuf};
use std::sync::mpsc::{self, Receiver, Sender};
use std::thread;
use std::time::Duration;

use anyhow::{Context, Result};
use notify_debouncer_full::notify::event::{AccessKind, AccessMode, MetadataKind, ModifyKind};
use notify_debouncer_full::notify::{
    EventKind, RecommendedWatcher, RecursiveMode, Watcher as _, WatcherKind,
};
use notify_debouncer_full::{
    DebounceEventResult, DebouncedEvent, Debouncer, RecommendedCache, new_debouncer,
};
use tokio::sync::mpsc::UnboundedSender;

use crate::cargo::Workspace;

/// How long the events of a file are gathered: an editor's save, a write then a rename, is one change.
const DEBOUNCE: Duration = Duration::from_millis(200);

/// The packages of the workspace and how they depend on each other, by their index in `Workspace::packages`.
#[derive(Debug, Clone)]
pub(crate) struct Graph {
    names: Vec<String>,
    /// The canonical directory of each package, as the paths of the watcher are matched in their canonical form.
    dirs: Vec<PathBuf>,
    /// The packages of the workspace each one is built with, directly.
    dependencies: Vec<Vec<usize>>,
}

/// What the watcher sends: the packages whose files changed, or its error.
#[derive(Debug)]
pub(crate) enum Watched {
    Changed(BTreeSet<usize>),
    Error(String),
}

/// The watcher, which watches until it is dropped.
pub(crate) struct Watcher {
    messages: Sender<Message>,
}

/// What the thread of the watcher receives: the events of the debouncer, or the end.
enum Message {
    Events(DebounceEventResult),
    Stop,
}

impl Drop for Watcher {
    fn drop(&mut self) {
        // A thread that ended, on an error of its own, has nothing left to stop.
        let _ = self.messages.send(Message::Stop);
    }
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
        let dependencies = workspace
            .packages
            .iter()
            .map(|package| {
                package
                    .path_dependencies()
                    .filter_map(|path| {
                        let path = canonical(path);

                        dirs.iter().position(|dir| *dir == path)
                    })
                    .collect()
            })
            .collect();

        Self {
            names,
            dirs,
            dependencies,
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

        let mut components = relative.components();

        if components
            .next()
            .is_some_and(|first| first.as_os_str() == "target")
        {
            return None;
        }

        if relative.components().any(hidden_or_modules) {
            return None;
        }

        if path.starts_with(&self.target) || self.outputs.iter().any(|output| output == path) {
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

    /// The directories of `dir`'s tree that count for `package`, `dir` first, which the rule leaves in. Another
    /// package's directory is left out: it is watched on its own when it counts. A directory that went away meanwhile
    /// is skipped.
    fn tree(&self, dir: &Path, package: usize) -> Vec<PathBuf> {
        let mut found = Vec::new();
        let mut next = vec![dir.to_path_buf()];

        while let Some(dir) = next.pop() {
            let Ok(entries) = fs::read_dir(&dir) else {
                continue;
            };

            for entry in entries.flatten() {
                // A link is not followed: its target is watched where it lies, when it counts.
                let is_dir = entry.file_type().is_ok_and(|kind| kind.is_dir());
                let path = entry.path();

                if is_dir && self.package(&path) == Some(package) {
                    next.push(path);
                }
            }

            found.push(dir);
        }

        found
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

/// How the directories are watched. inotify watches a directory at a time, with a limit on how many: each directory
/// the rule leaves in is watched alone, and those made or removed later are added or dropped. The other watchers,
/// FSEvents and Windows', watch a tree for the price of one, see a new directory by themselves, and restart their
/// stream at each directory added: a package is watched whole, the rule filtering its events.
fn one_by_one() -> bool {
    RecommendedWatcher::kind() == WatcherKind::Inotify
}

/// Watches the directories of `packages` and of those they are built with, and sends the packages whose files
/// change. It reads the file system: it is called before the runtime starts. It returns the watcher and how many
/// packages it watches.
pub(crate) fn start(
    workspace: &Workspace,
    graph: Graph,
    packages: impl IntoIterator<Item = usize>,
    sender: UnboundedSender<Watched>,
) -> Result<(Watcher, usize)> {
    let watched = graph.closure(packages);
    let config = &workspace.config;
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
        graph,
    };
    let (messages, received) = mpsc::channel();
    let events = messages.clone();
    let mut debouncer = new_debouncer(DEBOUNCE, None, move |result: DebounceEventResult| {
        // The thread ended when no one receives, and the watcher with it.
        let _ = events.send(Message::Events(result));
    })
    .context("cannot watch the workspace")?;
    let mut dirs = BTreeSet::new();

    for &package in &rule.watched {
        let root = rule.graph.dirs[package].clone();

        let watching = if one_by_one() {
            add(&mut debouncer, &rule.tree(&root, package), &mut dirs)
        } else {
            debouncer.watch(&root, RecursiveMode::Recursive)
        };

        watching.with_context(|| format!("cannot watch {}", root.display()))?;
    }

    let count = rule.watched.len();
    let mut thread = Thread {
        debouncer,
        rule,
        dirs,
        sender,
    };

    thread::Builder::new()
        .name("cargo-quoll-watch".into())
        .spawn(move || thread.run(&received))
        .context("cannot start the thread of the watcher")?;

    Ok((Watcher { messages }, count))
}

/// The thread of the watcher: it adds and drops the watches of directories, then sends the packages that changed,
/// so that a round starts once the directories it may read are watched.
struct Thread {
    debouncer: Debouncer<RecommendedWatcher, RecommendedCache>,
    rule: Rule,
    /// The directories watched one by one.
    dirs: BTreeSet<PathBuf>,
    sender: UnboundedSender<Watched>,
}

impl Thread {
    fn run(&mut self, received: &Receiver<Message>) {
        while let Ok(Message::Events(result)) = received.recv() {
            let watched = match result {
                Ok(events) => self.changed(&events),
                Err(errors) => Some(Watched::Error(
                    errors
                        .iter()
                        .map(ToString::to_string)
                        .collect::<Vec<_>>()
                        .join(", "),
                )),
            };

            if let Some(watched) = watched
                && self.sender.send(watched).is_err()
            {
                return;
            }
        }
    }

    /// The packages `events` change, once the directories they made or removed are watched or dropped.
    fn changed(&mut self, events: &[DebouncedEvent]) -> Option<Watched> {
        let mut changed = BTreeSet::new();
        let mut errors = Vec::new();

        for event in events {
            for path in &event.paths {
                let path = canonical(path);

                if one_by_one()
                    && let Err(error) = self.follow(&path)
                {
                    errors.push(format!("cannot watch {}: {error}", path.display()));
                }

                if changes(&event.kind)
                    && let Some(package) = self.rule.package(&path)
                {
                    changed.insert(package);
                }
            }
        }

        if !errors.is_empty() {
            let _ = self.sender.send(Watched::Error(errors.join(", ")));
        }

        (!changed.is_empty()).then_some(Watched::Changed(changed))
    }

    /// Watches the tree of a directory made, or moved into a package, and drops the watches of one removed, or moved
    /// out: whatever the event says, the file system is what is followed.
    fn follow(&mut self, path: &Path) -> notify_debouncer_full::notify::Result<()> {
        if path.is_dir() {
            if self.dirs.contains(path) {
                return Ok(());
            }

            let Some(package) = self.rule.package(path) else {
                return Ok(());
            };

            return add(
                &mut self.debouncer,
                &self.rule.tree(path, package),
                &mut self.dirs,
            );
        }

        if path.exists() {
            return Ok(());
        }

        let gone: Vec<_> = self
            .dirs
            .iter()
            .filter(|dir| dir.starts_with(path))
            .cloned()
            .collect();

        for dir in gone {
            // inotify drops the watch of a directory removed itself: there may be none left to drop.
            let _ = self.debouncer.unwatch(&dir);
            self.dirs.remove(&dir);
        }

        Ok(())
    }
}

/// Watches each of `dirs` alone.
fn add(
    debouncer: &mut Debouncer<RecommendedWatcher, RecommendedCache>,
    dirs: &[PathBuf],
    watched: &mut BTreeSet<PathBuf>,
) -> notify_debouncer_full::notify::Result<()> {
    for dir in dirs {
        if watched.insert(dir.clone()) {
            debouncer.watch(dir, RecursiveMode::NonRecursive)?;
        }
    }

    Ok(())
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
        }
    }

    fn rule(web: &str) -> Rule {
        Rule {
            graph: graph(),
            watched: BTreeSet::from([0, 1, 2, 4, 5]),
            target: PathBuf::from("/ws/target"),
            web: Some(PathBuf::from(web)),
            outputs: vec![PathBuf::from("/ws/api/openapi.json")],
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
    fn the_tree_of_a_package_holds_what_the_rule_leaves_in() {
        let dir = tempfile::tempdir().expect("a temporary directory");
        let root = canonical(dir.path());

        for path in [
            "server/src/target",
            "server/src/bin",
            "server/target/debug",
            "server/assets/node_modules/x",
            "server/.git/objects",
            "server/inner/src",
        ] {
            fs::create_dir_all(root.join(path)).expect("the directory is made");
        }

        let rule = Rule {
            graph: Graph {
                names: vec!["server".into(), "inner".into()],
                dirs: vec![root.join("server"), root.join("server/inner")],
                dependencies: vec![vec![], vec![]],
            },
            watched: BTreeSet::from([0, 1]),
            target: root.join("target"),
            web: None,
            outputs: vec![],
        };

        let mut tree = rule.tree(&root.join("server"), 0);
        tree.sort();

        let expected: Vec<_> = [
            "server",
            "server/assets",
            "server/src",
            "server/src/bin",
            "server/src/target",
        ]
        .iter()
        .map(|path| root.join(path))
        .collect();
        assert_eq!(tree, expected);
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
