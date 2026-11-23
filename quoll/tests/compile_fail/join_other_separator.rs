quoll::api! {
    pub api Api = "/api";

    pub group Issues {
        GET "/issues?labels={labels}" => list(#[join(";")] labels: Vec<String>);
    }
}

fn main() {}
