use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {}
    pub group NotesServer {}
    pub group ApiServices {}
}

fn main() {}
