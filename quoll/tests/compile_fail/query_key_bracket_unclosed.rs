quoll::api! {
    pub api Api = "/api";

    pub group Articles {
        GET "/articles?filter[name={name}" => list(name: Option<String>);
    }
}

fn main() {}
