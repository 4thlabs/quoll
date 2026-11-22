use quoll::rest;

rest::api! {
    #[openapi(none)]
    #[header("Content-Type" = "application/json; charset=utf-8")]
    pub api Api = "";

    pub group Files {
        POST "/files" => upload(file: rest::Bytes);
    }
}

fn main() {}
