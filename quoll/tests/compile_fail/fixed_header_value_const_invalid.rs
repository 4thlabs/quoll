/// A value with a line break, which no header holds.
const VERSION: &str = "2022-11-28\r\nX-Admin: 1";

quoll::api! {
    #[openapi(none)]
    #[header("X-Version" = VERSION)]
    pub api Api = "";
}

fn main() {}
