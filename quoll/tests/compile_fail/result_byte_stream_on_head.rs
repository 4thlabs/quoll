quoll::api! {
    pub api Api = "/api";

    pub group Health {
        HEAD "/" => check() -> quoll::ByteStream;
    }
}

fn main() {}
