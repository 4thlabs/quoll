quoll::group! {
    pub scheme Key = header "X-Api-Key";

    pub group Notes {
        GET "/notes" => list();
    }
}

fn main() {}
