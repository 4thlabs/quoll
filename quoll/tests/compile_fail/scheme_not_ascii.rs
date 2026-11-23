// A scheme name that is not ASCII, which `WWW-Authenticate` could not carry.
quoll::api! {
    pub api Api = "/api";

    pub scheme Clé = header "X-Api-Key";
}

fn main() {}
