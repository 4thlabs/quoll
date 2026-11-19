use futures_util::stream;
use quoll::rest::{self, Error, Events, JsonLines, Result};
use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize)]
pub struct Change {
    pub id: i64,
}

rest::api! {
    pub api Api = "/api";

    pub group Feeds {
        GET "/events" => events() -> Events<Change>;
        GET "/export?limit={limit}" => export(limit: Option<i64>) -> JsonLines<Change>;
    }
}

struct App;

// A stream of values, or of results whose error ends it.
impl FeedsServer for App {
    async fn events(&self) -> Result<Events<Change>> {
        Ok(Events::new(stream::iter([Change { id: 1 }])))
    }

    async fn export(&self, limit: Option<i64>) -> Result<JsonLines<Change>> {
        let limit = limit.ok_or_else(|| Error::bad_request("a limit is needed"))?;
        let changes = (1..=limit).map(|id| Ok(Change { id }));

        Ok(JsonLines::try_new(stream::iter(changes)))
    }
}

fn main() {
    let _router: axum::Router = Api::router(ApiServices { feeds: App });
}
