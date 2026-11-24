quoll::api! {
    #[openapi(none)]
    pub api Api = "";

    pub group Customers {
        POST "/customers?key={key}" => create(key: Option<String>, name: String) -> String;
    }
}

// An `Option<String>` takes `Some("x".into())`: an `Option<&str>` would make `None` ambiguous.
async fn create(api: &ApiClient) -> quoll::Result<String> {
    api.customers().create(Some("order-42"), "Ana").await
}

fn main() {
    let _ = create;
}
