quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/my notes" => list();
    }
}

fn main() {}
