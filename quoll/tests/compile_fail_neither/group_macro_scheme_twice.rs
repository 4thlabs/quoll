pub mod schemes {
    quoll::api! {
        pub api Api = "/api";

        pub scheme Key = header "X-Api-Key";
    }
}

use schemes::Key;

quoll::group! {
    #[auth(Key | schemes::Key)]
    pub group Notes {
        GET "/notes" => list();
    }
}

fn main() {}
