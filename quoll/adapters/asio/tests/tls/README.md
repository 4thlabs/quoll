# Certificates of the TLS tests

For the tests only: these private keys are public, and protect nothing. Valid for 100 years.

- `ca.pem`: the authority the tests trust (`ca_file`).
- `server.pem`, `server-key.pem`: a server named `localhost` and `127.0.0.1`.
- `other.pem`, `other-key.pem`: a server named `other.example`, signed by the same authority.

`generate.sh`, run in this directory, makes them again.
