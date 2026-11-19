use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        OPTIONS "/notes" => describe(note: NewNote);
    }
}

fn main() {}
