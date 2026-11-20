use quoll::rest;

// Items of the application's named as the ones the checks use.
#[allow(dead_code)]
struct InScope;
#[allow(dead_code)]
trait Group {}

fn main() {
    // A group and the api that lists it by its name, in one function body, as in a doctest.
    rest::group! {
        pub group Notes {
            GET "/notes" => list();
        }
    }

    rest::api! {
        pub api Api = "/api" { Notes };
    }

    let _api = Api;
}
