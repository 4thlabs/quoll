quoll::api! {
    pub api Api = "/api";

    pub group Issues {
        GET "/issues?labels={labels}" => list(#[join(",")] labels: Vec<Vec<String>>);
    }
}

fn main() {}
