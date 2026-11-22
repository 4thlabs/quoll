use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Issues {
        GET "/issues" => list(#[join(",")] labels: Vec<String>);
    }
}

fn main() {}
