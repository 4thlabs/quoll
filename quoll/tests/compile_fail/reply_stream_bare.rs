use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/events" => events() -> Events;
        GET "/lines"  => lines() -> rest::JsonLines<Note, Job>;
    }
}

fn main() {}
