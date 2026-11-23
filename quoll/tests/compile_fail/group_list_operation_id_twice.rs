mod tags {
    quoll::group! {
        pub group NoteTags {
            GET "/tags" => list();
        }
    }
}

quoll::api! {
    pub api Api = "/api" { tags::NoteTags };

    pub group Note {
        GET "/notes" => tags_list();
    }
}

fn main() {}
