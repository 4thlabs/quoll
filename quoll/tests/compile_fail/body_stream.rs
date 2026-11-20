use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        POST "/events" => send(events: rest::Events<Note>);
        POST "/lines"  => import(lines: JsonLines<Note>);
        POST "/logs"   => upload(logs: rest::ByteStream);
    }
}

fn main() {}
