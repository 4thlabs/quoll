use quoll::{Arg, Call};

quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

struct Fake;

// A fake's method takes the route's arguments, as `impl Arg<T>` of their types.
impl Notes for Fake {
    fn get(&self, id: impl Arg<String>) -> impl Call<String> {
        let id = id.into_value();

        async move {
            Ok(id)
        }
    }
}

fn main() {}
