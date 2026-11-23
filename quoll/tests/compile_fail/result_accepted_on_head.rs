quoll::api! {
    pub api Api = "/api";

    pub group Health {
        HEAD "/" => check() -> Accepted<Job>;
    }
}

fn main() {}
