use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        HEAD "/notes" => check(note: NewNote);
    }
}

fn main() {}
