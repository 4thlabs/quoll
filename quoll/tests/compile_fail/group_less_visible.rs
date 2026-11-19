use quoll::rest;

rest::api! {
    pub api Api = "/api";

    group Notes {
        GET "/notes/{id}" => get(id: i64);
    }

    pub(crate) group Tags {
        GET "/tags" => list();
    }

    pub group Health {
        HEAD "/" => check();
    }
}

fn main() {}
