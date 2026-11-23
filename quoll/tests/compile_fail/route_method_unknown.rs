quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        FETCH "/notes" => list();
    }
}

fn main() {}
