//! The config of `cargo quoll`, read from the workspace's `[workspace.metadata.quoll]`.

use std::fmt;
use std::path::PathBuf;

use serde::Deserialize;
use serde::de::{Deserializer, SeqAccess, Visitor};
use serde_json::Value;

use crate::cargo::Package;
use crate::commands::Command;

/// The key of the config in the workspace's `Cargo.toml`, which every error names first.
const ROOT: &str = "workspace.metadata.quoll";

/// The config of a workspace, checked against its packages.
#[derive(Debug)]
pub(crate) struct Config {
    pub(crate) apis: Vec<Api>,
    pub(crate) servers: Vec<Server>,
    pub(crate) web: Option<Web>,
}

/// A contract, whose binary prints its document.
#[derive(Debug)]
pub(crate) struct Api {
    /// Its key in the config, `workspace.metadata.quoll.api[0]`, which its errors name.
    pub(crate) key: String,
    pub(crate) package: String,
    pub(crate) bin: String,
    /// Relative to the workspace's root, as it is written.
    pub(crate) output: String,
    pub(crate) after: Vec<Command>,
}

/// A server, which `cargo quoll dev` builds and starts.
#[derive(Debug)]
pub(crate) struct Server {
    pub(crate) package: String,
    /// The binary `cargo run` would run: `default-run`, or the package's only one.
    pub(crate) bin: String,
    pub(crate) url: String,
    /// The `host:port` of `url`, which `cargo quoll dev` connects to, to know the server is ready.
    pub(crate) address: String,
}

/// The web side: its directory and its commands.
#[derive(Debug)]
pub(crate) struct Web {
    /// Its key in the config, which the errors of a missing command name.
    pub(crate) key: String,
    /// Relative to the workspace's root.
    pub(crate) dir: PathBuf,
    pub(crate) dev: Option<Command>,
    pub(crate) build: Option<Command>,
}

/// A config that cannot be read, with the key at fault.
#[derive(Debug)]
pub(crate) struct ConfigError {
    key: String,
    reason: Reason,
}

#[derive(Debug)]
enum Reason {
    /// What the key holds that cannot be, said by quoll.
    Message(String),
    /// What serde could not read at the key, its source.
    Serde(serde_json::Error),
}

impl ConfigError {
    pub(crate) fn new(key: impl Into<String>, message: impl Into<String>) -> Self {
        Self {
            key: key.into(),
            reason: Reason::Message(message.into()),
        }
    }
}

impl fmt::Display for ConfigError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match &self.reason {
            Reason::Message(message) => write!(f, "{}: {message}", self.key),
            Reason::Serde(_) => f.write_str(&self.key),
        }
    }
}

impl std::error::Error for ConfigError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match &self.reason {
            Reason::Message(_) => None,
            Reason::Serde(error) => Some(error),
        }
    }
}

impl Config {
    /// Reads the config from the workspace's `metadata`, as `cargo metadata` gives it, and checks it against the
    /// workspace's packages.
    pub(crate) fn read(metadata: &Value, packages: &[Package]) -> Result<Self, ConfigError> {
        let Some(quoll) = metadata.get("quoll") else {
            return Err(ConfigError::new(
                ROOT,
                "missing: cargo quoll reads its config there, in the workspace's Cargo.toml",
            ));
        };

        let raw: RawConfig = serde_path_to_error::deserialize(quoll).map_err(|error| {
            let path = error.path().to_string();
            let key = if path == "." {
                ROOT.to_owned()
            } else {
                format!("{ROOT}.{path}")
            };

            ConfigError {
                key,
                reason: Reason::Serde(error.into_inner()),
            }
        })?;

        let apis = raw
            .api
            .into_iter()
            .enumerate()
            .map(|(index, api)| read_api(format!("{ROOT}.api[{index}]"), api, packages))
            .collect::<Result<_, _>>()?;
        let servers = raw
            .server
            .into_iter()
            .enumerate()
            .map(|(index, server)| read_server(format!("{ROOT}.server[{index}]"), server, packages))
            .collect::<Result<_, _>>()?;
        let web = raw
            .web
            .map(|web| read_web(format!("{ROOT}.web"), web))
            .transpose()?;

        Ok(Self { apis, servers, web })
    }
}

fn read_api(key: String, raw: RawApi, packages: &[Package]) -> Result<Api, ConfigError> {
    let package = required(&key, "package", raw.package)?;

    let Some(found) = packages.iter().find(|candidate| candidate.name == package) else {
        return Err(no_package(&key, &package));
    };

    let bin = required(&key, "bin", raw.bin)?;

    if !found.has_binary(&bin) {
        return Err(ConfigError::new(
            format!("{key}.bin"),
            format!("`{package}` has no binary `{bin}`"),
        ));
    }

    let output = required(&key, "output", raw.output)?;

    if output.is_empty() {
        return Err(ConfigError::new(
            format!("{key}.output"),
            "empty: write the path of the document, from the workspace's root",
        ));
    }

    let after = raw
        .after
        .into_iter()
        .enumerate()
        .map(|(index, words)| command(format!("{key}.after[{index}]"), words))
        .collect::<Result<_, _>>()?;

    Ok(Api {
        key,
        package,
        bin,
        output,
        after,
    })
}

fn read_server(key: String, raw: RawServer, packages: &[Package]) -> Result<Server, ConfigError> {
    let package = required(&key, "package", raw.package)?;

    let Some(found) = packages.iter().find(|candidate| candidate.name == package) else {
        return Err(no_package(&key, &package));
    };

    let Some(bin) = found.default_binary() else {
        let binaries: Vec<_> = found.binaries().map(|bin| format!("`{bin}`")).collect();
        let message = if binaries.is_empty() {
            format!("`{package}` has no binary")
        } else {
            format!(
                "`{package}` has several binaries, {}: name the one to run with `default-run` in its Cargo.toml",
                binaries.join(", ")
            )
        };

        return Err(ConfigError::new(format!("{key}.package"), message));
    };

    let url = required(&key, "url", raw.url)?;
    let address =
        address(&url).map_err(|message| ConfigError::new(format!("{key}.url"), message))?;

    Ok(Server {
        package,
        bin: bin.to_owned(),
        url,
        address,
    })
}

/// The `host:port` an `http` or `https` url connects to, its port the scheme's when it names none.
fn address(url: &str) -> Result<String, String> {
    let (rest, port) = if let Some(rest) = url.strip_prefix("http://") {
        (rest, 80)
    } else if let Some(rest) = url.strip_prefix("https://") {
        (rest, 443)
    } else {
        return Err(format!(
            "`{url}` is not an http url: write the url the server answers on, `http://127.0.0.1:8080`"
        ));
    };

    let authority = rest.split(['/', '?', '#']).next().unwrap_or_default();
    // The credentials of a url are no part of where it connects.
    let host = authority.rsplit('@').next().unwrap_or_default();

    if host.is_empty() || host.starts_with(':') {
        return Err(format!("`{url}` names no host"));
    }

    match host.rsplit_once(':') {
        // The last `:` of an IPv6 address in brackets, `[::1]`, is in the address.
        Some((_, written)) if !written.contains(']') => {
            if written.parse::<u16>().is_err() {
                return Err(format!("`{url}`: `{written}` is not a port"));
            }

            Ok(host.to_owned())
        }
        _ => Ok(format!("{host}:{port}")),
    }
}

fn read_web(key: String, raw: RawWeb) -> Result<Web, ConfigError> {
    let dir = required(&key, "dir", raw.dir)?;

    if dir.is_empty() {
        return Err(ConfigError::new(
            format!("{key}.dir"),
            "empty: write the directory of the web side, from the workspace's root",
        ));
    }

    let dev = raw
        .dev
        .map(|words| command(format!("{key}.dev"), words))
        .transpose()?;
    let build = raw
        .build
        .map(|words| command(format!("{key}.build"), words))
        .transpose()?;

    Ok(Web {
        key,
        dir: dir.into(),
        dev,
        build,
    })
}

fn required<T>(key: &str, field: &str, value: Option<T>) -> Result<T, ConfigError> {
    value.ok_or_else(|| ConfigError::new(format!("{key}.{field}"), "missing"))
}

fn no_package(key: &str, package: &str) -> ConfigError {
    ConfigError::new(
        format!("{key}.package"),
        format!("no package `{package}` in the workspace"),
    )
}

fn command(key: String, words: Words) -> Result<Command, ConfigError> {
    if words.0.first().is_none_or(String::is_empty) {
        return Err(ConfigError::new(
            key,
            "empty: a command is its program, then its arguments",
        ));
    }

    Ok(Command::new(words.0, key))
}

/// The config as it is written, before its keys are checked.
#[derive(Deserialize)]
#[serde(deny_unknown_fields, expecting = "a table")]
struct RawConfig {
    #[serde(default)]
    api: Vec<RawApi>,
    #[serde(default)]
    server: Vec<RawServer>,
    web: Option<RawWeb>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields, expecting = "a table")]
struct RawApi {
    package: Option<String>,
    bin: Option<String>,
    output: Option<String>,
    #[serde(default)]
    after: Vec<Words>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields, expecting = "a table")]
struct RawServer {
    package: Option<String>,
    url: Option<String>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields, expecting = "a table")]
struct RawWeb {
    dir: Option<String>,
    dev: Option<Words>,
    build: Option<Words>,
}

/// A command as it is written: a list of words, whose error says so when it is a string.
struct Words(Vec<String>);

impl<'de> Deserialize<'de> for Words {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        struct WordsVisitor;

        impl<'de> Visitor<'de> for WordsVisitor {
            type Value = Words;

            fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                f.write_str(r#"a command, a list of words such as ["npm", "run", "build"]"#)
            }

            fn visit_seq<A: SeqAccess<'de>>(self, mut seq: A) -> Result<Words, A::Error> {
                let mut words = Vec::new();

                while let Some(word) = seq.next_element::<String>()? {
                    words.push(word);
                }

                Ok(Words(words))
            }
        }

        deserializer.deserialize_seq(WordsVisitor)
    }
}

#[cfg(test)]
mod tests {
    use serde_json::json;

    use super::*;

    fn packages() -> Vec<Package> {
        serde_json::from_value(json!([
            {
                "name": "notes-api",
                "id": "path+file:///notes/api#notes-api@0.1.0",
                "manifest_path": "/notes/api/Cargo.toml",
                "targets": [
                    { "name": "notes_api", "kind": ["lib"] },
                    { "name": "openapi", "kind": ["bin"] },
                ],
            },
            {
                "name": "notes-server",
                "id": "path+file:///notes/server#notes-server@0.1.0",
                "manifest_path": "/notes/server/Cargo.toml",
                "targets": [{ "name": "notes-server", "kind": ["bin"] }],
            },
            {
                "name": "notes-tools",
                "id": "path+file:///notes/tools#notes-tools@0.1.0",
                "manifest_path": "/notes/tools/Cargo.toml",
                "targets": [
                    { "name": "seed", "kind": ["bin"] },
                    { "name": "export", "kind": ["bin"] },
                ],
            },
            {
                "name": "notes-models",
                "id": "path+file:///notes/models#notes-models@0.1.0",
                "manifest_path": "/notes/models/Cargo.toml",
                "targets": [{ "name": "notes_models", "kind": ["lib"] }],
            },
        ]))
        .expect("the packages are those of cargo metadata")
    }

    fn read(quoll: Value) -> Result<Config, ConfigError> {
        Config::read(&json!({ "quoll": quoll }), &packages())
    }

    /// The error and its sources, as `cargo quoll` prints them.
    fn chain(error: &dyn std::error::Error) -> String {
        match error.source() {
            Some(source) => format!("{error}: {}", chain(source)),
            None => error.to_string(),
        }
    }

    fn error(quoll: Value) -> String {
        chain(&read(quoll).expect_err("the config is refused"))
    }

    fn api() -> Value {
        json!({
            "package": "notes-api",
            "bin": "openapi",
            "output": "web/openapi.json",
            "after": [["npx", "orval"], ["npm", "run", "lint"]],
        })
    }

    #[test]
    fn the_config_of_the_spec_is_read() {
        let config = read(json!({
            "api": [api()],
            "server": [{ "package": "notes-server", "url": "http://127.0.0.1:8080" }],
            "web": {
                "dir": "web",
                "dev": ["npm", "run", "dev"],
                "build": ["npm", "run", "build"],
            },
        }))
        .expect("the config is read");

        let [api] = config.apis.as_slice() else {
            panic!("one api: {:?}", config.apis);
        };
        assert_eq!(api.key, "workspace.metadata.quoll.api[0]");
        assert_eq!(api.package, "notes-api");
        assert_eq!(api.bin, "openapi");
        assert_eq!(api.output, "web/openapi.json");
        assert_eq!(
            api.after,
            [
                Command::new(
                    vec!["npx".into(), "orval".into()],
                    "workspace.metadata.quoll.api[0].after[0]".into()
                ),
                Command::new(
                    vec!["npm".into(), "run".into(), "lint".into()],
                    "workspace.metadata.quoll.api[0].after[1]".into()
                ),
            ]
        );
        let [server] = config.servers.as_slice() else {
            panic!("one server: {:?}", config.servers);
        };
        assert_eq!(server.package, "notes-server");
        assert_eq!(server.bin, "notes-server");
        assert_eq!(server.url, "http://127.0.0.1:8080");
        assert_eq!(server.address, "127.0.0.1:8080");

        let web = config.web.expect("the web side is read");

        assert_eq!(web.dir, PathBuf::from("web"));
        assert_eq!(
            web.build,
            Some(Command::new(
                vec!["npm".into(), "run".into(), "build".into()],
                "workspace.metadata.quoll.web.build".into()
            ))
        );
    }

    #[test]
    fn an_api_needs_no_after_and_the_config_no_web() {
        let config = read(json!({
            "api": [{ "package": "notes-api", "bin": "openapi", "output": "openapi.json" }],
        }))
        .expect("the config is read");

        assert!(config.apis[0].after.is_empty());
        assert!(config.web.is_none());
    }

    #[test]
    fn no_config_is_refused() {
        let error =
            chain(&Config::read(&Value::Null, &packages()).expect_err("no config is refused"));

        assert_eq!(
            error,
            "workspace.metadata.quoll: missing: cargo quoll reads its config there, in the workspace's Cargo.toml"
        );
    }

    #[test]
    fn a_config_that_is_not_a_table_is_refused() {
        assert_eq!(
            error(json!("api")),
            r#"workspace.metadata.quoll: invalid type: string "api", expected a table"#
        );
    }

    #[test]
    fn an_unknown_key_is_refused() {
        assert_eq!(
            error(json!({ "apis": [] })),
            "workspace.metadata.quoll.apis: unknown field `apis`, expected one of `api`, `server`, `web`"
        );

        let mut misspelt = api();

        misspelt["ouptut"] = misspelt["output"].take();
        assert_eq!(
            error(json!({ "api": [misspelt] })),
            "workspace.metadata.quoll.api[0].ouptut: unknown field `ouptut`, expected one of `package`, `bin`, `output`, `after`"
        );
    }

    #[test]
    fn a_missing_key_is_named() {
        for key in ["package", "bin", "output"] {
            let mut incomplete = api();

            incomplete
                .as_object_mut()
                .expect("an api is a table")
                .remove(key);

            assert_eq!(
                error(json!({ "api": [api(), incomplete] })),
                format!("workspace.metadata.quoll.api[1].{key}: missing")
            );
        }

        assert_eq!(
            error(json!({ "server": [{ "package": "notes-server" }] })),
            "workspace.metadata.quoll.server[0].url: missing"
        );
        assert_eq!(
            error(json!({ "web": { "build": ["npm", "run", "build"] } })),
            "workspace.metadata.quoll.web.dir: missing"
        );
    }

    #[test]
    fn a_key_of_the_wrong_type_is_named() {
        let mut api = api();

        api["output"] = json!(1);

        assert_eq!(
            error(json!({ "api": [api] })),
            "workspace.metadata.quoll.api[0].output: invalid type: integer `1`, expected a string"
        );
    }

    #[test]
    fn a_command_written_as_a_string_is_refused() {
        assert_eq!(
            error(json!({ "web": { "dir": "web", "build": "npm run build" } })),
            r#"workspace.metadata.quoll.web.build: invalid type: string "npm run build", expected a command, a list of words such as ["npm", "run", "build"]"#
        );

        let mut api = api();

        api["after"] = json!(["npx", "orval"]);
        assert_eq!(
            error(json!({ "api": [api] })),
            r#"workspace.metadata.quoll.api[0].after[0]: invalid type: string "npx", expected a command, a list of words such as ["npm", "run", "build"]"#
        );
    }

    #[test]
    fn a_word_that_is_not_a_string_is_named() {
        let mut api = api();

        api["after"] = json!([["npx", 1]]);

        assert_eq!(
            error(json!({ "api": [api] })),
            "workspace.metadata.quoll.api[0].after[0][1]: invalid type: integer `1`, expected a string"
        );
    }

    #[test]
    fn an_empty_command_is_refused() {
        let mut api = api();

        api["after"] = json!([["npx", "orval"], []]);
        assert_eq!(
            error(json!({ "api": [api] })),
            "workspace.metadata.quoll.api[0].after[1]: empty: a command is its program, then its arguments"
        );

        assert_eq!(
            error(json!({ "web": { "dir": "web", "dev": [""] } })),
            "workspace.metadata.quoll.web.dev: empty: a command is its program, then its arguments"
        );
    }

    #[test]
    fn a_serde_error_is_the_source() {
        let error = read(json!({ "apis": [] })).expect_err("the config is refused");

        assert_eq!(error.to_string(), "workspace.metadata.quoll.apis");
        assert_eq!(
            std::error::Error::source(&error)
                .map(ToString::to_string)
                .as_deref(),
            Some("unknown field `apis`, expected one of `api`, `server`, `web`")
        );
    }

    #[test]
    fn an_empty_web_dir_is_refused() {
        assert_eq!(
            error(json!({ "web": { "dir": "", "build": ["npm", "run", "build"] } })),
            "workspace.metadata.quoll.web.dir: empty: write the directory of the web side, from the workspace's root"
        );
    }

    #[test]
    fn an_empty_output_is_refused() {
        let mut api = api();

        api["output"] = json!("");

        assert_eq!(
            error(json!({ "api": [api] })),
            "workspace.metadata.quoll.api[0].output: empty: write the path of the document, from the workspace's root"
        );
    }

    #[test]
    fn a_package_outside_the_workspace_is_refused() {
        let mut api = api();

        api["package"] = json!("notes-ap");
        assert_eq!(
            error(json!({ "api": [api] })),
            "workspace.metadata.quoll.api[0].package: no package `notes-ap` in the workspace"
        );

        assert_eq!(
            error(json!({ "server": [{ "package": "notes", "url": "http://127.0.0.1:8080" }] })),
            "workspace.metadata.quoll.server[0].package: no package `notes` in the workspace"
        );
    }

    #[test]
    fn a_binary_the_package_does_not_have_is_refused() {
        let mut api = api();

        api["bin"] = json!("notes_api");

        assert_eq!(
            error(json!({ "api": [api] })),
            "workspace.metadata.quoll.api[0].bin: `notes-api` has no binary `notes_api`"
        );
    }

    #[test]
    fn a_server_runs_its_only_binary_or_its_default_run() {
        let servers = |package: &str| {
            read(json!({ "server": [{ "package": package, "url": "http://127.0.0.1:8080" }] }))
        };

        let error = chain(&servers("notes-tools").expect_err("the server is refused"));
        assert_eq!(
            error,
            "workspace.metadata.quoll.server[0].package: `notes-tools` has several binaries, `seed`, `export`: name the \
             one to run with `default-run` in its Cargo.toml"
        );

        let mut packages = packages();
        packages[2] = serde_json::from_value(json!({
            "name": "notes-tools",
            "id": "path+file:///notes/tools#notes-tools@0.1.0",
            "manifest_path": "/notes/tools/Cargo.toml",
            "targets": [{ "name": "seed", "kind": ["bin"] }, { "name": "export", "kind": ["bin"] }],
            "default_run": "export",
        }))
        .expect("the package is one of cargo metadata");
        let config = Config::read(
            &json!({ "quoll": { "server": [{ "package": "notes-tools", "url": "http://127.0.0.1:8080" }] } }),
            &packages,
        )
        .expect("the config is read");
        assert_eq!(config.servers[0].bin, "export");

        let error = chain(&servers("notes-models").expect_err("the server is refused"));
        assert_eq!(
            error,
            "workspace.metadata.quoll.server[0].package: `notes-models` has no binary"
        );
    }

    #[test]
    fn the_address_of_a_url_has_its_port() {
        assert_eq!(
            address("http://127.0.0.1:8080"),
            Ok("127.0.0.1:8080".into())
        );
        assert_eq!(address("http://localhost/api"), Ok("localhost:80".into()));
        assert_eq!(
            address("https://example.com?q#f"),
            Ok("example.com:443".into())
        );
        assert_eq!(
            address("http://user:secret@[::1]:3000/"),
            Ok("[::1]:3000".into())
        );
        assert_eq!(address("http://[::1]"), Ok("[::1]:80".into()));
    }

    #[test]
    fn a_url_that_does_not_connect_is_refused() {
        let url =
            |url: &str| error(json!({ "server": [{ "package": "notes-server", "url": url }] }));

        assert_eq!(
            url("127.0.0.1:8080"),
            "workspace.metadata.quoll.server[0].url: `127.0.0.1:8080` is not an http url: write the url the server \
             answers on, `http://127.0.0.1:8080`"
        );
        assert_eq!(
            url("http://:8080"),
            "workspace.metadata.quoll.server[0].url: `http://:8080` names no host"
        );
        assert_eq!(
            url("http://localhost:http"),
            "workspace.metadata.quoll.server[0].url: `http://localhost:http`: `http` is not a port"
        );
    }
}
