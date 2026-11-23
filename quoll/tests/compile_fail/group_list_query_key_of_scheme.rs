mod notes {
    use crate::Token;

    quoll::group! {
        #[auth(Token)]
        pub group Notes {
            GET "/notes?token={page}" => list(page: Option<String>);
        }
    }
}

quoll::api! {
    pub api Api = "/api" { notes::Notes };

    pub scheme Token = query "token";
}

fn main() {}
