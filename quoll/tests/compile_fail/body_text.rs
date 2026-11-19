use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        POST "/notes" => create(text: Text);
    }
}

fn main() {}
