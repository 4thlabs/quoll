use std::collections::HashMap;

use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct Version {
    pub version: String,
    pub api_version: String,
    #[serde(rename = "MinAPIVersion")]
    pub min_api_version: String,
    pub os: String,
    pub arch: String,
}

/// An event of `GET /events`: what happened, to what.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Event {
    /// `container`, `image`, `network`...
    #[serde(rename = "Type")]
    pub kind: String,
    /// `start`, `die`, `pull`...
    #[serde(rename = "Action")]
    pub action: String,
    #[serde(rename = "Actor")]
    pub actor: Actor,
    /// Seconds since the Unix epoch.
    pub time: i64,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct Actor {
    #[serde(rename = "ID")]
    pub id: String,
    /// The container's `name` and `image`, among others.
    #[serde(default)]
    pub attributes: HashMap<String, String>,
}
