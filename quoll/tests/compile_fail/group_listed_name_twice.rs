mod mine {
    quoll::group! {
        pub group Notes {
            GET "/mine" => list();
        }
    }
}

mod theirs {
    quoll::group! {
        pub group Notes {
            GET "/theirs" => list();
        }
    }
}

quoll::api! {
    pub api Api = "/api" { mine::Notes, theirs::Notes };
}

fn main() {}
