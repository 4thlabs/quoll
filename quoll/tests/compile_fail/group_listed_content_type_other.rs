use quoll::rest::{self, Form};

#[derive(serde::Serialize, serde::Deserialize)]
pub struct Login {
    pub user: String,
}

mod auth {
    use quoll::rest;

    rest::group! {
        #[openapi(none)]
        pub group Auth {
            POST "/token" => token(login: rest::Form<super::Login>) -> String;
        }
    }
}

rest::api! {
    // A form is not JSON.
    #[openapi(none)]
    #[header("Content-Type" = "application/json; charset=utf-8")]
    pub api Chat = "/api" { auth::Auth };
}

fn main() {
    let _ = Form(Login { user: String::new() });
}
