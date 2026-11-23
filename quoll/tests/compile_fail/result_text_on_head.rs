quoll::api! {
    pub api Api = "/api";

    pub group Health {
        HEAD "/" => check() -> Text;
    }
}

fn main() {}
