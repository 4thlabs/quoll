use serde::{Deserialize, Serialize};

/// A container as `GET /containers/json` lists it: the fields the command line shows.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct ContainerSummary {
    pub id: String,
    /// Each with a leading `/`, as Docker writes them.
    pub names: Vec<String>,
    pub image: String,
    /// `running`, `exited`...
    pub state: String,
    /// What `docker ps` shows: `Up 2 minutes`, `Exited (0) 3 hours ago`.
    pub status: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct ContainerInspect {
    pub id: String,
    pub name: String,
    pub created: String,
    pub config: ContainerConfig,
    pub state: ContainerState,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct ContainerConfig {
    pub image: String,
    /// Whether its output is a TTY's, which its logs send as they are rather than in frames.
    #[serde(default)]
    pub tty: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct ContainerState {
    pub status: String,
    pub running: bool,
    pub exit_code: i64,
    pub started_at: String,
    pub finished_at: String,
}

/// What `POST /containers/create` makes: the image and the command, with no TTY unless it says so.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct NewContainer {
    pub image: String,
    pub cmd: Vec<String>,
    pub tty: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct CreatedContainer {
    pub id: String,
    #[serde(default)]
    pub warnings: Vec<String>,
}
