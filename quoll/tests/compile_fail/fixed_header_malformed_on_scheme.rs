use quoll::rest;

rest::api! {
    #[openapi(none)]
    pub api Api = "";

    // A mistake of its own, beside the place it is at.
    #[header("X-Client" => "cli")]
    pub scheme Key = header "X-Api-Key";
}

fn main() {}
