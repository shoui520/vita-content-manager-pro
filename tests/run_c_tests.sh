#!/usr/bin/env sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT HUP INT TERM
cc=${CC:-cc}
flags="-std=gnu11 -Wall -Wextra -Werror"
vita="$root/src/vita"

build_run() {
    name=$1
    shift
    "$cc" $flags "$@" -o "$out/$name"
    "$out/$name"
}

build_run id3 "$vita/id3_tags.c" "$root/tests/test_id3_tags.c"
build_run catalog "$vita/mtp_catalog.c" "$root/tests/test_mtp_catalog.c"
build_run metadata "$vita/mtp_catalog.c" "$vita/mtp_wire.c" \
    "$vita/mtp_server.c" "$root/tests/test_mtp_metadata.c"
build_run vita-catalog "$vita/mtp_catalog.c" "$vita/tests/mtp_catalog_test.c"
build_run vita-wire "$vita/mtp_wire.c" "$vita/tests/mtp_wire_test.c"
build_run vita-server "$vita/mtp_catalog.c" "$vita/mtp_wire.c" \
    "$vita/mtp_server.c" "$vita/tests/mtp_server_test.c"
build_run vita-engine "$vita/mtp_catalog.c" "$vita/mtp_wire.c" \
    "$vita/mtp_server.c" "$vita/mtp_engine.c" "$vita/tests/mtp_engine_test.c"
build_run vita-thumb "$vita/mtp_thumb.c" "$vita/tests/mtp_thumb_test.c"
