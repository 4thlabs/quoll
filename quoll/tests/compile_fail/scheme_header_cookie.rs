use quoll::rest;

// A header scheme in `Cookie`, which a cookie scheme reads.
rest::api! {
    pub api Api = "/api";

    pub scheme Session = header "Cookie";
}

fn main() {}
