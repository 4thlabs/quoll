use quoll::rest;

rest::api! {
    #[openapi(none)]
    pub api Api = "";

    pub group Notes {
        // A `Created` answers JSON: the text is `-> Text`, given its 201 by the handler.
        POST "/notes" => create(title: String) -> rest::Created<rest::Text>;
    }
}

fn main() {}
