use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Notes {
        GET    "/notes/{id}"         => get(id: i64);
        DELETE "/notes/{note}"       => remove(note: i64);
        // A path that parts from the others after the parameter may name it otherwise.
        GET    "/notes/{key}/files"  => files(key: i64);
    }
}

fn main() {}
