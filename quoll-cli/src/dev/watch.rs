//! What `cargo quoll dev` watches: the directories of the packages its apis and servers are built from, and the
//! package a changed file belongs to.

use std::collections::BTreeSet;
use std::ffi::OsStr;
use std::fs;
use std::path::{Component, Path, PathBuf};
use std::sync::Arc;
use std::time::Duration;

use anyhow::{Context, Result};
use notify_debouncer_full::notify::event::{AccessKind, AccessMode, MetadataKind, ModifyKind};
use notify_debouncer_full::notify::{EventKind, RecommendedWatcher, RecursiveMode};
use notify_debouncer_full::{DebounceEventResult, Debouncer, RecommendedCache, new_debouncer};
use tokio::sync::mpsc::UnboundedSender;

use crate::cargo::Workspace;

/// How long the events of a file are gathered: an editor's save, a write then a rename, is one change.
const DEBOUNCE: Duration = Duration::from_millis(200);

/// The packages of the workspace and how they depend on each other, by their index in `Workspace::packages`.
#[derive(Debug)]
pub(crate) struct Graph {
    names: Vec<String>,
    /// The directory of each package, then its canonical form when it is another: a watcher may give either.
    dirs: Vec<Vec<PathBuf>>,
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
    _debouncer: Debouncer<RecommendedWatcher, RecommendedCache>,
}

impl Graph {
    pub(crate) fn new(workspace: &Workspace) -> Self {
        let names = workspace
            .packages
            .iter()
            .map(|package| package.name.clone())
            .collect();
        let dirs: Vec<_> = workspace
            .packages
            .iter()
            .map(|package| both_forms(package.dir()))
            .collect();
        let dependencies = workspace
            .packages
            .iter()
            .map(|package| {
                package
                    .path_dependencies()
                    .filter_map(|path| {
                        let path = both_forms(path);

                        dirs.iter()
                            .position(|forms| forms.iter().any(|dir| path.contains(dir)))
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

    /// The package whose directory holds `path`, the innermost when packages are nested, with the path from it.
    fn package_of<'a>(&self, path: &'a Path) -> Option<(usize, &'a Path)> {
        self.dirs
            .iter()
            .enumerate()
            .flat_map(|(index, forms)| forms.iter().map(move |dir| (index, dir)))
            .filter_map(|(index, dir)| Some((index, dir, path.strip_prefix(dir).ok()?)))
            .max_by_key(|(_, dir, _)| dir.components().count())
            .map(|(index, _, relative)| (index, relative))
    }
}

/// What the watcher reports, and what it leaves out.
#[derive(Debug)]
struct Filter {
    graph: Graph,
    watched: BTreeSet<usize>,
    /// The target directory, the web side and the documents, which `cargo quoll dev` writes or which watch
    /// themselves.
    ignored: Vec<PathBuf>,
}

impl Filter {
    /// The watched package a change of `path` is a change of, none when the change does not count.
    fn package(&self, path: &Path) -> Option<usize> {
        if self.ignored.iter().any(|ignored| path.starts_with(ignored)) {
            return None;
        }

        let (package, relative) = self.graph.package_of(path)?;

        if !self.watched.contains(&package) || relative.components().any(skipped) {
            return None;
        }

        Some(package)
    }
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

/// Whether a part of a path inside a package leaves it out: a hidden file or directory, `.git` or an editor's
/// swap file, or a `target` of its own.
fn skipped(component: Component<'_>) -> bool {
    let Component::Normal(name) = component else {
        return false;
    };

    name == "target" || name.to_string_lossy().starts_with('.')
}

/// Watches the directories of `packages` and of those they are built with, and sends the packages whose files
/// change, except the documents, the web side and the target directory.
pub(crate) fn start(
    workspace: &Workspace,
    packages: impl IntoIterator<Item = usize>,
    sender: UnboundedSender<Watched>,
) -> Result<(Watcher, usize)> {
    let graph = Graph::new(workspace);
    let watched = graph.closure(packages);
    let config = &workspace.config;
    let mut ignored: Vec<PathBuf> = vec![workspace.target_directory.clone()];

    ignored.extend(config.web.iter().map(|web| workspace.root.join(&web.dir)));
    ignored.extend(
        config
            .apis
            .iter()
            .map(|api| workspace.root.join(&api.output)),
    );

    let ignored = ignored.iter().flat_map(|path| both_forms(path)).collect();
    let filter = Arc::new(Filter {
        graph,
        watched,
        ignored,
    });
    let handler = Arc::clone(&filter);

    let mut debouncer = new_debouncer(DEBOUNCE, None, move |result: DebounceEventResult| {
        let watched = match result {
            Ok(events) => Watched::Changed(
                events
                    .iter()
                    .filter(|event| changes(&event.kind))
                    .flat_map(|event| &event.paths)
                    .filter_map(|path| handler.package(path))
                    .collect(),
            ),
            Err(errors) => Watched::Error(
                errors
                    .iter()
                    .map(ToString::to_string)
                    .collect::<Vec<_>>()
                    .join(", "),
            ),
        };

        if matches!(&watched, Watched::Changed(packages) if packages.is_empty()) {
            return;
        }

        // `cargo quoll dev` is stopping when no one receives.
        let _ = sender.send(watched);
    })
    .context("cannot watch the workspace")?;

    for &package in &filter.watched {
        let dir = &filter.graph.dirs[package][0];

        watch_dir(&mut debouncer, dir, &filter)
            .with_context(|| format!("cannot watch {}", dir.display()))?;
    }

    let count = filter.watched.len();

    Ok((
        Watcher {
            _debouncer: debouncer,
        },
        count,
    ))
}

/// Watches the files of `dir`, and its directories but those a change in does not count, so that the target
/// directory or the `node_modules` of a package at the workspace's root are not watched, file by file, for nothing.
fn watch_dir(
    debouncer: &mut Debouncer<RecommendedWatcher, RecommendedCache>,
    dir: &Path,
    filter: &Filter,
) -> Result<()> {
    debouncer.watch(dir, RecursiveMode::NonRecursive)?;

    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        let path = entry.path();
        let name = entry.file_name();

        if !entry.file_type()?.is_dir() || name == OsStr::new("node_modules") {
            continue;
        }

        let left_out = skipped(Component::Normal(&name))
            || filter.ignored.iter().any(|ignored| path.starts_with(ignored))
            // Another package is watched on its own, when it counts.
            || filter.graph.dirs.iter().flatten().any(|other| *other == path);

        if !left_out {
            debouncer.watch(&path, RecursiveMode::Recursive)?;
        }
    }

    Ok(())
}

/// `path`, then its canonical form when it is another, as a watcher on macOS gives `/private/var` for `/var`.
fn both_forms(path: &Path) -> Vec<PathBuf> {
    let mut forms = vec![path.to_path_buf()];

    if let Ok(canonical) = fs::canonicalize(path)
        && canonical != path
    {
        forms.push(canonical);
    }

    forms
}

#[cfg(test)]
mod tests {
    use serde_json::json;

    use super::*;
    use crate::cargo::Package;

    fn package(name: &str, dir: &str, dependencies: &[&str]) -> Package {
        serde_json::from_value(json!({
            "name": name,
            "id": format!("path+file://{dir}#{name}@0.1.0"),
            "manifest_path": format!("{dir}/Cargo.toml"),
            "targets": [],
            "dependencies": dependencies
                .iter()
                .map(|path| json!({ "name": "any", "kind": null, "path": path }))
                .chain([json!({ "name": "test-tools", "kind": "dev", "path": "/ws/tools" })])
                .collect::<Vec<_>>(),
        }))
        .expect("the package is one of cargo metadata")
    }

    fn graph() -> Graph {
        let packages = [
            package("models", "/ws/models", &[]),
            package("api", "/ws/api", &["/ws/models"]),
            package("server", "/ws/server", &["/ws/api"]),
            package("tools", "/ws/tools", &[]),
            package("root", "/ws", &[]),
        ];

        Graph {
            names: packages
                .iter()
                .map(|package| package.name.clone())
                .collect(),
            dirs: packages
                .iter()
                .map(|package| vec![package.dir().to_path_buf()])
                .collect(),
            dependencies: vec![vec![], vec![0], vec![1], vec![], vec![]],
        }
    }

    #[test]
    fn a_read_is_no_change() {
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
        assert!(changes(&EventKind::Remove(
            notify_debouncer_full::notify::event::RemoveKind::File
        )));
    }

    #[test]
    fn a_server_depends_on_the_crates_of_its_contract() {
        let graph = graph();

        assert!(graph.depends_on(2, 2));
        assert!(graph.depends_on(2, 1));
        assert!(graph.depends_on(2, 0));
        assert!(!graph.depends_on(1, 2));
        assert!(
            !graph.depends_on(2, 3),
            "a dev-dependency is not built with it"
        );
        assert_eq!(graph.closure([2]), BTreeSet::from([0, 1, 2]));
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
        let filter = Filter {
            graph: graph(),
            watched: BTreeSet::from([0, 1, 2]),
            ignored: vec![
                PathBuf::from("/ws/target"),
                PathBuf::from("/ws/api/openapi.json"),
            ],
        };

        assert_eq!(filter.package(Path::new("/ws/api/src/lib.rs")), Some(1));
        assert_eq!(filter.package(Path::new("/ws/api/openapi.json")), None);
        assert_eq!(
            filter.package(Path::new("/ws/api/.openapi.json.12.tmp")),
            None
        );
        assert_eq!(
            filter.package(Path::new("/ws/server/src/.main.rs.swp")),
            None
        );
        assert_eq!(
            filter.package(Path::new("/ws/server/target/debug/server")),
            None
        );
        assert_eq!(filter.package(Path::new("/ws/target/debug/server")), None);
        assert_eq!(
            filter.package(Path::new("/ws/tools/src/main.rs")),
            None,
            "not watched"
        );
    }
}
