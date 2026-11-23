use futures_util::stream;
use quoll::{ByteStream, Error, Events, JsonLines, Response, Result};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, JsonSchema)]
pub struct Change {
    pub id: i64,
}

quoll::api! {
    pub api Api = "/api";

    pub group Feeds {
        GET "/events" => events() -> Events<Change>;
        GET "/export?limit={limit}" => export(limit: Option<i64>) -> JsonLines<Change>;
        GET "/logs" => logs() -> ByteStream;
    }
}

struct App;

// A stream of values, or of results whose error ends it.
impl FeedsServer for App {
    async fn events(&self) -> Result<Response<Events<Change>>> {
        Ok(Response::new(Events::new(stream::iter([Change { id: 1 }]))))
    }

    async fn export(&self, limit: Option<i64>) -> Result<Response<JsonLines<Change>>> {
        let limit = limit.ok_or_else(|| Error::bad_request("a limit is needed"))?;
        let changes = (1..=limit).map(|id| Ok(Change { id }));

        Ok(Response::new(JsonLines::try_new(stream::iter(changes))))
    }

    async fn logs(&self) -> Result<Response<ByteStream>> {
        let chunks = [Ok(b"a line\n".to_vec()), Err(Error::internal("the process died"))];

        Ok(Response::new(ByteStream::try_new(stream::iter(chunks)).with_content_type("text/plain")))
    }
}

fn main() {
    let _router: axum::Router = Api::router(ApiServices { feeds: App });
}
