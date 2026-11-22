use quoll::rest;

rest::api! {
    #[openapi(none)]
    #[header("anthropic-beta" = "files-api-2025-04-14")]
    pub api Api = "/v1";

    pub group Messages {
        // A typo: the api fixes `anthropic-beta`.
        #[header("anthropic-bta" = none)]
        POST "/messages/count_tokens" => count_tokens(text: String) -> u32;
    }
}

fn main() {}
