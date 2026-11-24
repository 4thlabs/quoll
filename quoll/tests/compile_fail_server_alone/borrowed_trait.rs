/// A trait whose objects the client can send: the server cannot receive one owned.
pub trait Shape {}

impl serde::Serialize for dyn Shape {
    fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.serialize_unit()
    }
}

quoll::api! {
    #[openapi(none)]
    pub api Api = "/api";

    pub group Shapes {
        POST "/shapes" => create(shape: &dyn Shape);
    }
}

fn main() {}
