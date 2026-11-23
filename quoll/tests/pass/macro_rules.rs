// A type handed over by a `macro_rules!` arrives in an invisible group, and the contract reads it as written.
macro_rules! contract {
    ($reply:ty, $id:ty) => {
        quoll::api! {
            pub api Api = "/api";

            pub group Notes {
                POST "/notes" => create(note: String) -> $reply;
                GET "/notes/{id}" => get(id: $id) -> String;
            }
        }
    };
}

contract!(quoll::Created<String>, u64);

fn main() {
    let _api = Api;
}
