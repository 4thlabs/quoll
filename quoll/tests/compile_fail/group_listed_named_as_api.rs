mod other {
    quoll::group! {
        pub group Api {
            GET "/notes" => list();
        }
    }
}

use other::*;

quoll::api! {
    pub api Api = "/api" { Api };
}

fn main() {}
