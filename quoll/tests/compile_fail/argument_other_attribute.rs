quoll::api! {
    pub api Api = "/api";

    pub group Issues {
        GET "/issues?labels={labels}" => list(#[serde(default)] labels: Vec<String>);
    }
}

fn main() {}
