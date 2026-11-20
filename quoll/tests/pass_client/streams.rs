use futures_util::{StreamExt, stream};
use quoll::rest::{self, ByteStream, Events, JsonLines, Result};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Debug, Serialize, Deserialize, JsonSchema)]
pub struct Change {
    pub id: i64,
}

rest::api! {
    pub api Api = "/api";

    pub group Feeds {
        GET "/events" => events() -> Events<Change>;
        GET "/export" => export() -> JsonLines<Change>;
        GET "/logs"   => logs() -> ByteStream;
    }
}

// A caller reads a stream as the spec's command line does, whoever implements the trait.
async fn watch(feeds: &impl Feeds) -> Result<()> {
    let mut events = feeds.events().await?;

    while let Some(event) = events.next().await {
        println!("{:?}", event?);
    }

    let mut lines = feeds.export().await?;

    while let Some(line) = lines.next().await {
        println!("{:?}", line?);
    }

    let mut logs = feeds.logs().await?;

    println!("{}", logs.content_type());
    while let Some(chunk) = logs.next().await {
        println!("{}", String::from_utf8_lossy(&chunk?));
    }

    Ok(())
}

// A fake gives the same types, built from values.
struct Fake;

impl Feeds for Fake {
    async fn events(&self) -> Result<Events<Change>> {
        Ok(Events::new(stream::iter([Change { id: 1 }])))
    }

    async fn export(&self) -> Result<JsonLines<Change>> {
        Ok(JsonLines::new(stream::empty()))
    }

    async fn logs(&self) -> Result<ByteStream> {
        Ok(ByteStream::new(stream::iter([b"a line\n".to_vec()])).with_content_type("text/plain"))
    }
}

fn is_send<T: Send>(_: T) {}

fn main() -> Result<()> {
    let api = Api::client("http://localhost:8080")?;

    is_send(watch(&api.feeds()));
    is_send(watch(&Fake));
    Ok(())
}
