use quoll::Call;

quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: i64) -> String;
    }
}

struct Fake;

// A fake's method takes the route's arguments, of their types.
impl Notes for Fake {
    fn get(&self, id: String) -> impl Call<String> {
        async move {
            Ok(id)
        }
    }
}

fn main() {}
