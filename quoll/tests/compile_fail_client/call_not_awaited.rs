#![deny(unused_must_use)]

use quoll::rest;

rest::api! {
    #[openapi(none)]
    pub api Api = "";

    pub group Notes {
        DELETE "/notes/{id}" => remove(id: i64);
    }
}

// A call not awaited sends nothing.
async fn forget(api: &ApiClient) {
    api.notes().remove(3);
}

fn main() {
    let _ = forget;
}
