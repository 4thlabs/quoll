use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        PUT "/notes" => write(note: NewNote, text: Text);
    }
}

fn main() {}
