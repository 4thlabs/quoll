use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Exports {
        POST "/exports" => export(name: String) -> Accepted;
    }
}

fn main() {}
