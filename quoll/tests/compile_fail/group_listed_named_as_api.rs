use quoll::rest;

mod other {
    use quoll::rest;

    rest::group! {
        pub group Api {
            GET "/notes" => list();
        }
    }
}

use other::*;

rest::api! {
    pub api Api = "/api" { Api };
}

fn main() {}
