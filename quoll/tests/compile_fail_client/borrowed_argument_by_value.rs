mod api {
    quoll::api! {
        pub api Api = "/api";

        pub group Repos {
            GET "/repos/{owner}?sort={sort}" => get(owner: &str, sort: Option<&str>) -> String;
        }
    }
}

// Only the client's trait is in scope, so that the message is the same whatever the features.
use api::Repos;

// The client borrows an argument the contract writes `&str`: a `String` of the caller's is lent, `&owner`.
async fn call(repos: &impl Repos, owner: String) -> quoll::Result<String> {
    repos.get(owner, None).await
}

fn main() {}
