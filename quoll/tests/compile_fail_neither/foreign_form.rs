use quoll::rest;
use serde::{Deserialize, Serialize};

// A `Form` of the application's own, which the contract reads as quoll's.
mod forms {
    pub struct Form<T>(pub T);
}

use forms::Form;

#[derive(Serialize, Deserialize)]
pub struct Login {
    pub user: String,
}

rest::api! {
    pub api Api = "/api";

    pub group Auth {
        POST "/token" => token(login: Form<Login>);
    }
}

fn main() {}
