use quoll::rest;

rest::api! {
    api Api = "";

    group Health {
        HEAD "/" => check();
        GET "/health" => status() -> String;
    }
}

fn main() {
    let _api = Api;
}
