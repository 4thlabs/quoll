// A header that HTTP keeps to one connection.
quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes" => list(header("Connection") connection: String);
    }
}

fn main() {}
