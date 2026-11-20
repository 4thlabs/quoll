use quoll::rest;

mod notes {
    use quoll::rest;

    use crate::Token;

    rest::group! {
        #[auth(Token)]
        pub group Notes {
            GET "/notes?token={page}" => list(page: Option<String>);
        }
    }
}

rest::api! {
    pub api Api = "/api" { notes::Notes };

    pub scheme Token = query "token";
}

fn main() {}
