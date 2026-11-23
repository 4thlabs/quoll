// A header scheme in `Authorization`, which `Bearer` and `Basic` read.
quoll::api! {
    pub api Api = "/api";

    pub scheme Key = header "Authorization";
}

fn main() {}
