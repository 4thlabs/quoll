use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Repos {
        #[auth(Bearer, scopes("repo"))]
        DELETE "/repos/{id}" => remove(id: i64);
    }
}

fn main() {}
