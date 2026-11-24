quoll::api! {
    pub api Api = "/api";

    pub group Repos {
        GET "/repos/{owner}" => get(owner: &str) -> String;
    }
}

// A call may borrow its arguments until it is awaited, so that a fake reads them in its `async move`: an argument
// lives as long as the call.
async fn call(repos: &impl Repos) -> quoll::Result<String> {
    let call = {
        let owner = String::from("ana");

        repos.get(&owner)
    };

    call.await
}

fn main() {}
