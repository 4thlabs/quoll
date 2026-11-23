use quoll::Form;

#[derive(serde::Serialize, serde::Deserialize)]
pub struct Login {
    pub user: String,
}

mod auth {
    quoll::group! {
        #[openapi(none)]
        pub group Auth {
            POST "/token" => token(login: quoll::Form<super::Login>) -> String;
        }
    }
}

quoll::api! {
    // A form is not JSON.
    #[openapi(none)]
    #[header("Content-Type" = "application/json; charset=utf-8")]
    pub api Chat = "/api" { auth::Auth };
}

fn main() {
    let _ = Form(Login { user: String::new() });
}
