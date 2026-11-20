use quoll::rest;

mod mine {
    use quoll::rest;

    rest::group! {
        pub group Notes {
            GET "/mine" => list();
        }
    }
}

mod theirs {
    use quoll::rest;

    rest::group! {
        pub group Notes {
            GET "/theirs" => list();
        }
    }
}

rest::api! {
    pub api Api = "/api" { mine::Notes, theirs::Notes };
}

fn main() {}
