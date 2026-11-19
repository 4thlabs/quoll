//! The client calls an `https` URL as it is: the feature `client` brings TLS.

#![cfg(feature = "client")]

use quoll::rest::{self, Result};
use tokio::io::AsyncReadExt;

rest::api! {
    api Api = "";

    group Health {
        GET "/health" => check();
    }
}

#[tokio::test]
async fn an_https_url_opens_a_tls_handshake() -> Result<()> {
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0")
        .await
        .expect("a port is free");
    let address = listener.local_addr().expect("the listener has an address");
    let (first_byte, mut received) = tokio::sync::oneshot::channel();

    // Reads what the client sends first, then closes: the handshake fails, after it has begun.
    tokio::spawn(async move {
        let (mut stream, _) = listener.accept().await.expect("the client connects");
        let byte = stream.read_u8().await.expect("the client sends a byte");
        let _ = first_byte.send(byte);
    });

    let error = Api::client(&format!("https://{address}"))?
        .health()
        .check()
        .await
        .expect_err("the server closes the connection");

    // The byte is sent before the connection closes, so before the call fails; without TLS, no connection is made.
    assert_eq!(error.status(), None);
    // 0x16: a TLS record of the handshake, the ClientHello.
    assert_eq!(received.try_recv().ok(), Some(0x16), "{error:?}");
    Ok(())
}
