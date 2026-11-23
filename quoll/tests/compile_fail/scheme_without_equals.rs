// A scheme without its `=`.
quoll::api! {
    pub api Api = "/api";

    pub scheme Key header "X-Api-Key";
}

fn main() {}
