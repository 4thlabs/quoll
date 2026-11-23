pub mod notes {
    quoll::group! {
        /// The signed-in user's notes.
        #[auth(Bearer | crate::Key)]
        pub group Notes {
            GET "/notes/{id}" => get(id: i64) -> String;
        }
    }
}

pub mod auth {
    quoll::group! {
        pub group Auth {
            POST "/token" => token() -> String;
        }
    }
}

pub mod third_party {
    quoll::group! {
        // A group of an api that is only called, whose types need no schema.
        #[openapi(none)]
        pub group Repos {
            GET "/repos" => list() -> Vec<u8>;
        }
    }

    quoll::api! {
        #[openapi(none)]
        pub api GitHub = "" { Repos };
    }
}

// A group named alone, which a glob import brings.
use auth::*;

quoll::api! {
    /// Notes, in modules.
    pub api Api = "/api" { Auth, crate::notes::Notes };

    pub scheme Key = header "X-Api-Key";

    pub group Health {
        HEAD "/" => check();
    }
}

// The same group, under another prefix.
quoll::api! {
    pub api Mirror = "/mirror" { notes::Notes };
}

fn main() {
    let _apis = (Api, Mirror, third_party::GitHub);
}
