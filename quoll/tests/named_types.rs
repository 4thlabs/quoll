//! A contract with no document, built without the feature `client`: no code of a side names its types, and the macro
//! names them itself, so that each import is used. Its types are ones clippy reads in a signature, `ptr_arg` on a
//! `&String` and `box_collection` on a `Box<String>`: CI's clippy, with `-D warnings`, runs on this file with no
//! feature and with `server`, and checks that the macro's naming of them is not the user's to fix. With `client`, the
//! client's trait takes them as the contract writes them, and clippy reads them there, as it does in any contract.

#![cfg(not(feature = "client"))]

use std::collections::BTreeMap;

quoll::api! {
    /// An API only called, whose client this build leaves out.
    #[server(none)]
    pub api Tags = "/tags";

    pub group Labels {
        POST "/labels?name={name}" => set(name: &String, note: Box<String>) -> BTreeMap<String, String>;
    }
}
