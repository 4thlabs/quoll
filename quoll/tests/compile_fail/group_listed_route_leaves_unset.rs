use quoll::rest;

mod messages {
    use quoll::rest;

    rest::group! {
        #[openapi(none)]
        pub group Messages {
            #[header("anthropic-beta" = none)]
            POST "/messages/count_tokens" => count_tokens(text: String) -> u32;
        }
    }
}

rest::api! {
    #[openapi(none)]
    #[header("anthropic-version" = "2023-06-01")]
    pub api Anthropic = "/v1" { messages::Messages };
}

fn main() {}
