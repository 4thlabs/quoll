#!/bin/sh
# Makes the certificates of the TLS tests again (see README.md), in the current directory.
set -e

days=36500

openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout ca-key.pem -out ca.pem \
  -days "$days" -subj "/CN=Quoll test authority"

issue() {
  openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout "$1-key.pem" -out "$1.csr" -subj "/CN=$2"
  printf 'subjectAltName=%s\n' "$3" > "$1.ext"
  openssl x509 -req -in "$1.csr" -CA ca.pem -CAkey ca-key.pem -CAcreateserial -out "$1.pem" -days "$days" \
    -extfile "$1.ext"
  rm "$1.csr" "$1.ext"
}

issue server localhost "DNS:localhost,IP:127.0.0.1"
issue other other.example "DNS:other.example"

rm ca-key.pem ca.srl
