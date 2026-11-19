use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {}
    pub group NotesServer {}
    pub group ApiServices {}
    pub group NotesClient {}
    pub group ApiClient {}
}

fn main() {}
