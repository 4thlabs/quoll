use quoll::rest;

rest::api! {
    pub api Api = "/api";

    pub group Jobs {
        GET "/jobs/{job}" => get(job: Accepted<u64>) -> String;
    }
}

fn main() {}
