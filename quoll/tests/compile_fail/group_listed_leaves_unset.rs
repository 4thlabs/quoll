use quoll::rest;

mod messages {
    use quoll::rest;

    rest::group! {
        #[openapi(none)]
        #[header("anthropic-beta" = none)]
        pub group Messages {
            POST "/messages" => create(text: String) -> u32;
        }
    }
}

rest::api! {
    // The api fixes no `anthropic-beta`, which the group leaves.
    #[openapi(none)]
    #[header("anthropic-version" = "2023-06-01")]
    pub api Anthropic = "/v1" { messages::Messages };
}

fn main() {}
