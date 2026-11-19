//! Prints the OpenAPI document of the notes contract, which the generators of TypeScript read.

fn main() {
    println!("{}", notes_api::Api::openapi().to_json_pretty());
}
