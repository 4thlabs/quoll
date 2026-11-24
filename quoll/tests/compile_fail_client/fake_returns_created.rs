use quoll::{Arg, Call, Created};

quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        POST "/notes" => create(title: String) -> Created<String>;
    }
}

// On the client, a `Created<T>` route gives the `T`: the 201 is the server's answer only.
struct Fake;

impl Notes for Fake {
    fn create(&self, title: impl Arg<String>) -> impl Call<Created<String>> {
        let title = title.into_value();

        async move { Err(quoll::Error::not_found(title)) }
    }
}

fn main() {}
