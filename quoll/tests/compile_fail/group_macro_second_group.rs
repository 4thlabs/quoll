use quoll::rest;

rest::group! {
    pub group Notes {
        GET "/notes" => list();
    }

    pub group Tags {
        GET "/tags" => list();
    }
}

fn main() {}
