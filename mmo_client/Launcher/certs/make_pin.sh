#!/bin/bash
# Print the pin line for a server certificate.
#
# Usage: ./make_pin.sh /path/to/server.crt
#
# The pin covers the SubjectPublicKeyInfo, not the certificate, so it survives a
# certificate renewal that keeps the same key pair. Append the output to
# whichever pin file names that server:
#
#   the login server  -> Launcher/certs/login_pins.txt
#   the realm server  -> Game/certs/realm_pins.txt
#
# This pipeline is the authority for what a pin is; the C side is checked
# against it by Game/tests/cert_pin_digest_test.c.
set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: $0 <server.crt>" >&2
    exit 2
fi

CERT="$1"
if [ ! -r "$CERT" ]; then
    echo "cannot read $CERT" >&2
    exit 1
fi

DIGEST=$(openssl x509 -in "$CERT" -pubkey -noout \
         | openssl pkey -pubin -outform der \
         | openssl dgst -sha256 -binary \
         | openssl base64)

echo "sha256/${DIGEST}"
