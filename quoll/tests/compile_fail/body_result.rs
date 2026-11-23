quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        POST "/notes" => create(note: Created<Note>) -> Note;
        PUT  "/jobs"  => run(job: quoll::Accepted<Job>);
    }
}

fn main() {}
