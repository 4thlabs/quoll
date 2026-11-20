fn main() {
    println!("{}", notes_api::Api::openapi().to_json_pretty());
}
