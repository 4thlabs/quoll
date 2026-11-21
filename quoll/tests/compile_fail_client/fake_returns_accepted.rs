use quoll::rest::{self, Accepted, Call};

rest::api! {
    pub api Api = "/api";

    pub group Exports {
        POST "/exports" => export(name: String) -> Accepted<String>;
    }
}

// On the client, an `Accepted<T>` route gives the `T`: the 202 is the server's answer only.
struct Fake;

impl Exports for Fake {
    fn export(&self, name: String) -> impl Call<Accepted<String>> {
        async move { Err(rest::Error::not_found(name)) }
    }
}

fn main() {}
