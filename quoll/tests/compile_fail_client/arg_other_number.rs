quoll::api! {
    #[openapi(none)]
    pub api Api = "";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

// No number becomes another: an `i32` for an `i64` is written `i64::from(id)`.
async fn get(api: &ApiClient, id: i32) -> quoll::Result<String> {
    api.notes().get(id).await
}

fn main() {
    let _ = get;
}
