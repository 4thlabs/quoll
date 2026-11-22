use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Issues {
        GET "/issues/{ids}" => get(#[join(",")] ids: Vec<i64>);
    }
}

fn main() {}
