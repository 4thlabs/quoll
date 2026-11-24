use quoll::{Accepted, Arg, Call};

quoll::api! {
    pub api Api = "/api";

    pub group Exports {
        POST "/exports" => export(name: String) -> Accepted<String>;
    }
}

// On the client, an `Accepted<T>` route gives the `T`: the 202 is the server's answer only.
struct Fake;

impl Exports for Fake {
    fn export(&self, name: impl Arg<String>) -> impl Call<Accepted<String>> {
        let name = name.into_value();

        async move { Err(quoll::Error::not_found(name)) }
    }
}

fn main() {}
