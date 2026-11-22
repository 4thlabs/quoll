use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Issues {
        GET "/issues?labels={labels}" => list(#[join(",")] #[join("|")] labels: Vec<String>);
    }
}

fn main() {}
