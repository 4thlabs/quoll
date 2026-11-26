use quoll::auth::{Outgoing, Unsendable};
use quoll::{ClientScheme, Scheme};

/// A scheme a client writes, which no server reads.
pub struct MediaBrowser;

impl Scheme for MediaBrowser {
    const NAME: &'static str = "MediaBrowser";
}

impl ClientScheme for MediaBrowser {
    type Secret = String;

    fn write(key: &String, request: &mut Outgoing<'_>) -> Result<(), Unsendable> {
        request.authorization_params("MediaBrowser", &[("Token", key)])
    }
}

quoll::group! {
    #[auth(crate::MediaBrowser)]
    pub group Library {
        GET "/Items" => items();
    }
}

fn main() {}
