quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/events" => events() -> Events;
        GET "/lines"  => lines() -> quoll::JsonLines<Note, Job>;
    }
}

fn main() {}
