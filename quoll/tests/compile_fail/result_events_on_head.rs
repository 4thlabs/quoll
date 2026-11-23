quoll::api! {
    pub api Api = "/api";

    pub group Health {
        HEAD "/" => check() -> Events<String>;
    }
}

fn main() {}
