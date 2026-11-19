use quoll::rest;

// An alternative of more schemes than the crate takes.
rest::api! {
    pub api Api = "/api";

    pub scheme A = header "X-A";
    pub scheme B = header "X-B";
    pub scheme C = header "X-C";
    pub scheme D = header "X-D";

    #[auth(Bearer | A | B | C | D)]
    pub group Notes {
        GET "/notes" => list();
    }
}

fn main() {}
