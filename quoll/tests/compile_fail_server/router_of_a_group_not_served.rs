mod notes {
    quoll::group! {
        #[server(none)]
        pub group Notes {
            GET "/notes/{id}" => get(id: i64) -> String;
        }
    }
}

quoll::api! {
    /// Served, with no document.
    #[openapi(none)]
    pub api Api = "/api" { notes::Notes };
}

struct App;

fn main() {
    let _ = Api::router(ApiServices { notes: App });
}
