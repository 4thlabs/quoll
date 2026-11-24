use quoll::{Arg, Call};

quoll::api! {
    pub api Api = "/api";

    #[auth(Bearer)]
    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

struct User;
struct Fake;

// The client's trait has no caller: its middleware sends the credential.
impl Notes for Fake {
    fn get(&self, _user: &User, id: impl Arg<i64>) -> impl Call<String> {
        let id = id.into_value();

        async move {
            Ok(id.to_string())
        }
    }
}

fn main() {}
