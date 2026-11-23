// An OpenID Connect scheme without the URL of its discovery document.
quoll::api! {
    pub api Api = "/api";

    pub scheme Login = openid "accounts.example.com";
}

fn main() {}
