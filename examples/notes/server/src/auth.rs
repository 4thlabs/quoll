use notes_api::{AuthServer, Key, Login, TokenResponse};
use quoll::rest::{Authenticate, Bearer, Error, Form, Result};

use crate::App;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct User {
    pub id: i64,
    pub name: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Operator {
    pub name: String,
}

impl AuthServer for App {
    async fn token(&self, login: Form<Login>) -> Result<TokenResponse> {
        let user = self
            .store
            .check_password(&login.user, &login.password)
            .await
            .map_err(Error::internal)?
            .ok_or_else(|| Error::unauthorized("wrong user or password"))?;

        Ok(TokenResponse {
            access_token: self.sessions.open(user),
        })
    }
}

impl Authenticate<Bearer> for App {
    type Caller = User;

    async fn authenticate(&self, token: String) -> Result<User> {
        self.sessions
            .user(&token)
            .ok_or_else(|| Error::unauthorized("unknown or expired token"))
    }
}

impl Authenticate<Key> for App {
    type Caller = Operator;

    async fn authenticate(&self, key: String) -> Result<Operator> {
        self.store
            .operator(&key)
            .ok_or_else(|| Error::unauthorized("unknown key"))
    }
}
