// A scheme named as the word of a route that takes no credential.
quoll::api! {
    pub api Api = "/api";

    pub scheme none = header "X-Token";
}

fn main() {}
