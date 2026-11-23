quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        POST "/events" => send(events: quoll::Events<Note>);
        POST "/lines"  => import(lines: JsonLines<Note>);
        POST "/logs"   => upload(logs: quoll::ByteStream);
    }
}

fn main() {}
