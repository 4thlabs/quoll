// A scheme named as one of the crate's.
quoll::api! {
    pub api Api = "/api";

    pub scheme Bearer = header "X-Token";
}

fn main() {}
