use quoll::rest;

mod notes {
    pub struct Notes;
}

rest::api! {
    pub api Api = "/api" { notes::Notes };
}

fn main() {}
