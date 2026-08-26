#!/bin/sh
#
# Fail when the shared pinning sources differ between the two trees.
#
# Four links in this system are identified by a public-key pin, and both ends of
# each one have to compute the same 32 bytes from the same certificate. The code
# that does that -- parsing a pin file, and digesting a certificate's DER
# SubjectPublicKeyInfo -- therefore exists in both trees and must be identical.
#
# This is not a hypothetical. Every pinning site in both trees once called
# X509_pubkey_digest(), which hashes the public key bit string rather than the
# SubjectPublicKeyInfo, and so disagreed with every pin file the setup script
# writes. It was invisible because both ends were wrong in the same way. One
# copy of the computation, checked here, is what stops that shape of bug: a
# divergence becomes a failed check rather than a link that quietly refuses
# every peer.
#
# Point MMO_CLIENT_ROOT at the client tree, or check it out beside this one.
# Missing entirely, this skips; found and different, it fails.

set -u

cd "$(dirname "$0")/.." || exit 1
SERVER_ROOT=$(pwd)

CLIENT_ROOT="${MMO_CLIENT_ROOT:-$SERVER_ROOT/../mmo_client}"

if [ ! -d "$CLIENT_ROOT" ]; then
    printf '  client tree not found at %s — skipping\n' "$CLIENT_ROOT"
    printf '  set MMO_CLIENT_ROOT to check the two trees against each other\n'
    exit 0
fi

# server path | client path
PAIRS="common/include/cert_pin.h:common/cert_pin.h
common/src/cert_pin.c:common/cert_pin.c
common/include/cert_spki.h:common/cert_spki.h
common/src/cert_spki.c:common/cert_spki.c
tests/cert_pin_digest_test.c:Game/tests/cert_pin_digest_test.c"

status=0

for pair in $PAIRS; do
    server_rel="${pair%%:*}"
    client_rel="${pair##*:}"
    server_file="$SERVER_ROOT/$server_rel"
    client_file="$CLIENT_ROOT/$client_rel"

    if [ ! -f "$server_file" ]; then
        printf '  %-34s MISSING in the server tree\n' "$server_rel"
        status=1
        continue
    fi
    if [ ! -f "$client_file" ]; then
        printf '  %-34s MISSING in the client tree (%s)\n' "$server_rel" "$client_rel"
        status=1
        continue
    fi

    if diff -q "$server_file" "$client_file" >/dev/null 2>&1; then
        printf '  %-34s ok\n' "$server_rel"
    else
        printf '  %-34s DIFFERS from %s:\n' "$server_rel" "$client_rel"
        diff "$server_file" "$client_file" | sed 's/^/      /' | head -20
        status=1
    fi
done

if [ "$status" -ne 0 ]; then
    printf '\nThese files are one implementation with two copies on disk. Both ends of\n'
    printf 'a pinned link must agree byte for byte about how a pin is computed, or\n'
    printf 'the link refuses every peer while both sides believe they are correct.\n'
    printf 'Copy the corrected version over the other and re-run.\n'
    exit 1
fi

printf '  the pinning sources are identical in both trees\n'
exit 0
