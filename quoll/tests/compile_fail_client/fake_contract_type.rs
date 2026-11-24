use quoll::Call;

quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

struct Fake;

// A fake's method takes `impl Arg<i64>`, as the trait does, not the contract's type alone.
impl Notes for Fake {
    fn get(&self, id: i64) -> impl Call<String> {
        async move {
            Ok(id.to_string())
        }
    }
}

fn main() {}
