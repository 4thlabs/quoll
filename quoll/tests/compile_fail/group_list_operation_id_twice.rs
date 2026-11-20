use quoll::rest;

mod tags {
    use quoll::rest;

    rest::group! {
        pub group NoteTags {
            GET "/tags" => list();
        }
    }
}

rest::api! {
    pub api Api = "/api" { tags::NoteTags };

    pub group Note {
        GET "/notes" => tags_list();
    }
}

fn main() {}
