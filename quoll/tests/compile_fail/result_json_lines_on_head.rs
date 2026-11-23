quoll::api! {
    pub api Api = "/api";

    pub group Health {
        HEAD "/" => check() -> quoll::JsonLines<String>;
    }
}

fn main() {}
