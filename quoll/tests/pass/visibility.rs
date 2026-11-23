// A group reaches at least as far as its api.
quoll::api! {
    pub(crate) api Api = "";

    pub(crate) group Notes {
        GET "/notes" => list();
    }

    pub group Health {
        HEAD "/" => check();
    }
}

fn main() {
    let _api = Api;
}
