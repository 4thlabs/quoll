mod rest_shapes {
    use std::fmt;

    use serde::Deserialize;

    // The application's own `Problem`, which is not quoll's: a group without a shape takes quoll's.
    #[derive(Debug, Deserialize)]
    pub struct Problem {
        pub message: String,
    }

    impl fmt::Display for Problem {
        fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
            f.write_str(&self.message)
        }
    }
}

mod gists {
    quoll::group! {
        #[openapi(none)]
        pub group Gists {
            GET "/gists" => list();
        }
    }
}

use rest_shapes::Problem;

quoll::api! {
    #[openapi(none)]
    #[error(Problem)]
    pub api Hub = "" { gists::Gists };
}

fn main() {}
