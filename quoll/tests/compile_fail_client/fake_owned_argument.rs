use quoll::Call;

quoll::api! {
    pub api Api = "/api";

    pub group Repos {
        GET "/repos/{owner}?sort={sort}" => get(owner: &str, sort: Option<&str>) -> String;
    }
}

struct Fake;

// A fake's method takes the client's types, `&str` where the contract writes it, and not the server's `String`.
impl Repos for Fake {
    fn get(&self, owner: String, _sort: Option<String>) -> impl Call<String> {
        async move { Ok(owner) }
    }
}

fn main() {}
