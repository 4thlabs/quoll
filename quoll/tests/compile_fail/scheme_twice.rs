// Two schemes of one name.
quoll::api! {
    pub api Api = "/api";

    pub scheme Key = header "X-Api-Key";
    pub scheme Key = header "X-Other-Key";
}

fn main() {}
