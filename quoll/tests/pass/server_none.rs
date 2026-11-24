//! A contract of a third-party API, only called, built with any features of quoll: no server side, no document, and
//! each import the contract names used.

#![deny(unused_imports)]

use std::collections::BTreeMap;
use std::net::IpAddr;

use quoll::ByteStream;
use serde::{Deserialize, Serialize};

// The types need no schema, nor what only a server would read or write: a body `Serialize` alone, a result
// `Deserialize` alone.
#[derive(Debug, Serialize)]
pub struct NewRepo {
    pub name: String,
}

#[derive(Debug, Deserialize)]
pub struct Repo {
    pub full_name: String,
}

mod issues {
    quoll::group! {
        /// A group only called, which the api lists.
        #[server(none)]
        pub group Issues {
            GET "/repos/{owner}/{repo}/issues" => list(owner: &str, repo: &str) -> Vec<String>;
            // The path of `Repos::get`, its parameters named otherwise: only a router takes them for one.
            PUT "/repos/{user}/{name}" => rename(user: &str, name: &str);
        }
    }
}

mod stars {
    quoll::group! {
        /// A group of both sides, which an api only called lists too.
        pub group Stars {
            GET "/user/starred" => list() -> Vec<String>;
        }
    }
}

quoll::api! {
    /// GitHub's API, which the application only calls.
    #[server(none)]
    pub api GitHub = "" { issues::Issues, stars::Stars };

    pub group Repos {
        GET "/repos/{owner}/{repo}" => get(owner: &str, repo: &str) -> Repo;
        DELETE "/repos/{org}/{project}" => remove(org: &str, project: &str);
        POST "/user/repos" => create(repo: &NewRepo) -> Repo;
        GET "/repos/{owner}/{repo}/labels" => labels(owner: &str, repo: &str) -> BTreeMap<String, String>;
        GET "/meta/{ip}" => meta(ip: IpAddr) -> ByteStream;
    }
}

// The names of the server's items are the application's, whatever the features of quoll: the contract writes none.
pub trait ReposServer {}

impl GitHub {
    pub fn router() {}
}

fn main() {
    GitHub::router();
}
