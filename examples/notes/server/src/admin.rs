use notes_api::{AdminServer, Stats};
use quoll::rest::Result;

use crate::{App, Operator};

impl AdminServer for App {
    async fn stats(&self, _operator: &Operator) -> Result<Stats> {
        Ok(self.store.stats())
    }
}
