use quoll::{Arg, Call};

quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET    "/notes/{id}" => get(id: i64) -> String;
        DELETE "/notes/{id}" => remove(id: i64);
    }
}

struct Fake;

impl Notes for Fake {
    fn get(&self, _id: impl Arg<i64>) -> impl Call<String> {
        async move {
            Ok(String::new())
        }
    }
}

fn main() {}
