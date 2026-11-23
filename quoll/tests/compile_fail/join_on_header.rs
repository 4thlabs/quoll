quoll::api! {
    pub api Api = "/api";

    pub group Issues {
        GET "/issues" => list(#[join(",")] header("X-Labels") labels: Vec<String>);
    }
}

fn main() {}
