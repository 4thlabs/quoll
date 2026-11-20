use quoll::rest;

pub mod schemes {
    use quoll::rest;

    rest::api! {
        pub api Api = "/api";

        pub scheme Key = header "X-Api-Key";
    }
}

use schemes::Key;

rest::group! {
    #[auth(Key | schemes::Key)]
    pub group Notes {
        GET "/notes" => list();
    }
}

fn main() {}
