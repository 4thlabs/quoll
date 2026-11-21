use notes_api::{AdminServer, Stats};
use quoll::rest::{Response, Result};

use crate::{App, Operator};

impl AdminServer for App {
    async fn stats(&self, _operator: &Operator) -> Result<Response<Stats>> {
        Ok(Response::new(self.store.stats()))
    }
}
