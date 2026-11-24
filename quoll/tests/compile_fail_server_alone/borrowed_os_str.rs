// The server receives `X` for a `&X`, which `OsStr`, of no size, cannot be: rustc says so on the contract's type, among
// the errors of the server's code that names it.
quoll::api! {
    pub api Api = "/api";

    pub group Files {
        GET "/files?name={name}" => read(name: &std::ffi::OsStr) -> String;
    }
}

fn main() {}
