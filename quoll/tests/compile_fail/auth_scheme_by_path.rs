// A declared scheme named by a path too, which would take it twice.
quoll::api! {
    pub api Api = "/api";

    pub scheme Key = header "X-Api-Key";

    #[auth(Key | self::Key)]
    pub group Admin {
        GET "/stats" => stats();
    }
}

fn main() {}
