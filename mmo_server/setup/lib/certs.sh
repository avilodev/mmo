# Generate every TLS identity in the system and cross-install the pins.
# Sourced by setup.sh.
#
# Three links are encrypted, and each end of each one has to be told who to
# expect. Nothing here is issued by a CA -- these are self-signed certificates
# on machines the operator owns -- so identity is a SHA-256 fingerprint over the
# DER SubjectPublicKeyInfo, provisioned ahead of time and checked after every
# handshake.
#
#   launcher --> login    launcher pins login
#   game     --> realm    game pins realm
#   realm   <--> world    realm pins world AND world pins realm
#
# The game <-> world link is plaintext by design; see common/include/tls.h.
#
# Six files come out of this, in three pairs plus three pin lists:
#
#   login_server/certs/server.{crt,key}      mmo_client/Launcher/certs/login_pins.txt
#   realm_server/certs/server.{crt,key}      mmo_client/Game/certs/realm_pins.txt
#   world_server/certs/server.{crt,key}      realm_server/certs/world_pins.txt
#                                            world_server/certs/realm_pins.txt
#
# The public key rather than the certificate is what is pinned, so a
# certificate can be renewed on the same key pair without reissuing anything.

# Where the client tree lives, if it is checked out beside this one.
CLIENT_ROOT="${MMO_CLIENT_ROOT:-$(cd "$PROJECT_ROOT/.." 2>/dev/null && pwd)/mmo_client}"

# service | common name
CERT_SERVICES="login_server:mmo-login realm_server:mmo-realm world_server:mmo-world"

# ---------------------------------------------------------------------------
# Generation
# ---------------------------------------------------------------------------

# Generate one service's key pair and self-signed certificate, if it needs one.
# $1 service directory name, $2 certificate common name
generate_one_certificate() {
    local service="$1" cn="$2"
    local dir="$PROJECT_ROOT/$service/certs"
    local crt="$dir/server.crt" key="$dir/server.key"

    mkdir -p "$dir"

    if [ -f "$crt" ] && [ -f "$key" ]; then
        local expiry
        expiry=$(openssl x509 -in "$crt" -noout -enddate 2>/dev/null | cut -d= -f2)
        if openssl x509 -in "$crt" -noout -checkend 0 >/dev/null 2>&1; then
            same "$service/certs/server.crt present, valid until ${expiry:-unknown}"
            return 0
        fi
        warn "$service/certs/server.crt has expired (${expiry:-unknown}); generating a new one"
        note "    every pin naming it must be regenerated too — this run does that"
    fi

    openssl req -x509 -newkey rsa:2048 \
        -keyout "$key" -out "$crt" \
        -days 3650 -nodes -subj "/CN=$cn" >/dev/null 2>&1 \
        || { blocker "openssl could not generate the $service certificate"; return 1; }

    chmod 600 "$key"
    made "$service/certs/server.{crt,key} (self-signed, CN=$cn, 10 years)"
}

generate_certificates() {
    step "TLS certificates"

    local entry service cn rc=0
    for entry in $CERT_SERVICES; do
        service="${entry%%:*}"
        cn="${entry##*:}"
        generate_one_certificate "$service" "$cn" || rc=1
    done

    note "self-signed on purpose: every peer here is identified by public-key pin,"
    note "not by a certificate chain, so there is no CA to obtain one from"
    return $rc
}

# ---------------------------------------------------------------------------
# Pinning
# ---------------------------------------------------------------------------

# Compute the pin line for a certificate: SHA-256 over the DER
# SubjectPublicKeyInfo, base64. The public key rather than the certificate, so
# the pin survives a certificate renewal that keeps the same key pair.
# $1 path to the certificate
compute_pin_line() {
    local digest
    digest=$(openssl x509 -in "$1" -pubkey -noout 2>/dev/null \
             | openssl pkey -pubin -outform der 2>/dev/null \
             | openssl dgst -sha256 -binary 2>/dev/null \
             | openssl base64 2>/dev/null)
    [ -n "$digest" ] || return 1
    printf 'sha256/%s\n' "$digest"
}

# Write or extend a pin file so it names a certificate.
#
# An existing file is appended to rather than replaced. That is how a key is
# rotated without stranding a build already in someone's hands: both pins are
# accepted until the old build is gone.
#
# $1 pin file path, $2 certificate to pin, $3 human description of the link
install_pin() {
    local pin_file="$1" crt="$2" what="$3"
    local pin

    if [ ! -f "$crt" ]; then
        warn "no certificate at $crt — cannot write $what"
        return 1
    fi

    pin=$(compute_pin_line "$crt") || { warn "could not fingerprint $crt"; return 1; }

    mkdir -p "$(dirname "$pin_file")"

    if [ -f "$pin_file" ] && grep -qxF "$pin" "$pin_file"; then
        same "$what already pins this key"
        return 0
    fi

    if [ -f "$pin_file" ]; then
        cp "$pin_file" "$pin_file.bak"
        printf '\n# Added by setup/setup.sh on %s\n%s\n' "$(date -Iseconds)" "$pin" >> "$pin_file"
        made "appended to $what (previous kept, backup at .bak)"
        note "    the older pin still works; drop it once nothing carries it"
        return 0
    fi

    cat > "$pin_file" <<PINFILE
# Public-key pins: $what
#
# A TLS connection on this link is accepted only when the peer's public key
# fingerprints to one of the lines below. Nothing else identifies that peer:
# these certificates are self-signed, so there is no chain to verify and no CA
# to trust.
#
# Format: sha256/<base64 of SHA-256 over the DER SubjectPublicKeyInfo>
# Produce one with:
#   openssl x509 -in server.crt -pubkey -noout \\
#     | openssl pkey -pubin -outform der \\
#     | openssl dgst -sha256 -binary | openssl base64
#
# More than one line may be listed. That is how a key is rotated: publish a
# build carrying both the current and the next pin, roll the server, then drop
# the old line.
#
# A malformed line is a startup failure, not a skipped line -- a pin file the
# operator got wrong must never quietly shrink to fewer pins than intended, and
# an empty set accepts nobody rather than everybody.

# Generated by setup/setup.sh on $(date -Iseconds)
# from $crt
$pin
PINFILE
    made "wrote $what"
}

# The two server-side pin files, which never need the client tree.
install_server_pins() {
    step "Realm and world pins"

    install_pin "$PROJECT_ROOT/realm_server/certs/world_pins.txt" \
                "$PROJECT_ROOT/world_server/certs/server.crt" \
                "realm_server/certs/world_pins.txt (the worlds this realm will talk to)"

    install_pin "$PROJECT_ROOT/world_server/certs/realm_pins.txt" \
                "$PROJECT_ROOT/realm_server/certs/server.crt" \
                "world_server/certs/realm_pins.txt (the realms this world will accept)"

    note "both ends of the realm<->world link check the other, so both files matter:"
    note "a world with no realm_pins.txt refuses every realm, and a realm with no"
    note "world_pins.txt connects to nothing"
}

# The two client-side pin files, which need the client tree.
install_client_pins() {
    step "Client pins"

    if [ ! -d "$CLIENT_ROOT" ]; then
        warn "client tree not found at $CLIENT_ROOT — cannot install the client pins"
        note "    The launcher refuses every login connection, and the game refuses"
        note "    every realm connection, unless their pin files name these keys."
        note "    On the machine that builds the client, run:"
        note "      Launcher/certs/login_pins.txt  <- $(compute_pin_line "$PROJECT_ROOT/login_server/certs/server.crt" 2>/dev/null || echo '?')"
        note "      Game/certs/realm_pins.txt      <- $(compute_pin_line "$PROJECT_ROOT/realm_server/certs/server.crt" 2>/dev/null || echo '?')"
        note "    Set MMO_CLIENT_ROOT=/path/to/mmo_client to have setup do it."
        return 0
    fi

    install_pin "$CLIENT_ROOT/Launcher/certs/login_pins.txt" \
                "$PROJECT_ROOT/login_server/certs/server.crt" \
                "Launcher/certs/login_pins.txt (the login server)"

    install_pin "$CLIENT_ROOT/Game/certs/realm_pins.txt" \
                "$PROJECT_ROOT/realm_server/certs/server.crt" \
                "Game/certs/realm_pins.txt (the realm server)"

    note "rebuild the client for these to take effect"
}
