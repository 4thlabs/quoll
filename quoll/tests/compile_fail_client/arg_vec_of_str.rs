quoll::api! {
    #[openapi(none)]
    pub api Api = "";

    pub group Issues {
        GET "/issues?labels={labels}" => list(labels: Vec<String>) -> Vec<String>;
    }
}

// A list of `&str` is an array, `["C-bug"]`: a `Vec<&str>` would make `vec![]` ambiguous.
async fn list(api: &ApiClient) -> quoll::Result<Vec<String>> {
    api.issues().list(vec!["C-bug"]).await
}

fn main() {
    let _ = list;
}
