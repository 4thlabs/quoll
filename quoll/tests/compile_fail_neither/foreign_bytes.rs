use quoll::rest;

// A `Bytes` of another crate, which the contract reads as quoll's.
mod other {
    pub struct Bytes(pub Vec<u8>);
}

rest::api! {
    pub api Api = "/api";

    pub group Files {
        PUT "/files/{id}" => upload(id: i64, file: other::Bytes);
    }
}

fn main() {}
