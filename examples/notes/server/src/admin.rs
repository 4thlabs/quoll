use notes_api::{AdminServer, Stats};
use quoll::rest::{Error, Result};

use crate::{App, Operator};

impl AdminServer for App {
    async fn stats(&self, _operator: &Operator) -> Result<Stats> {
        self.store.stats().await.map_err(Error::internal)
    }
}
