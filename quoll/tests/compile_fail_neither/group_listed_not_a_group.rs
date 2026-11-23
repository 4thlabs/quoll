mod notes {
    pub struct Notes;
}

quoll::api! {
    pub api Api = "/api" { notes::Notes };
}

fn main() {}
