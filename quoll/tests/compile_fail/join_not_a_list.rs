use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Issues {
        GET "/issues?labels={labels}" => list(#[join(",")] labels: String);
    }
}

fn main() {}
