quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes/{id}" => get(id: Events<i64>);
        GET "/lines" => lines(header("X-Lines") lines: JsonLines<i64>);
        GET "/logs/{logs}" => logs(logs: ByteStream);
    }
}

fn main() {}
