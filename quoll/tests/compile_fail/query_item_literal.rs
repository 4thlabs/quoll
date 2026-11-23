quoll::api! {
    pub api Api = "/api";

    pub group Notes {
        GET "/notes?page=1" => list();
    }
}

fn main() {}
