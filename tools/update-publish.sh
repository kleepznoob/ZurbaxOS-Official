#!/bin/sh
# Prepares a Zurbax security update for the release channel.
#
#   tools/update-publish.sh KEY.pem VERSION BASE-URL "NOTES" OUT-DIR FILE...
#       writes OUT-DIR/release (the update's description, listing each FILE
#       with its size and SHA-256) and OUT-DIR/release.sig, signed with KEY.pem
#   tools/update-publish.sh --new-key KEY.pem
#       makes a signing key and prints the line for data/update-keys/<name>.pub
#
# FILEs are the update's zurbax.efi (built with make OS_UPDATE=N) and any
# layer images (*.sfs) that changed; upload them to BASE-URL, and release
# with release.sig to the channel, https://www.zurbax.com/updates/<release>/.
# Keep KEY.pem private and out of this repository.
set -eu
if [ "${1:-}" = "--new-key" ]; then
	openssl ecparam -name prime256v1 -genkey -noout -out "$2"
	chmod 600 "$2"
	echo "public key (put this line in data/update-keys/<name>.pub):"
	openssl ec -in "$2" -pubout -outform DER 2>/dev/null | tail -c 65 | od -An -tx1 | tr -d ' \n'
	echo
	exit 0
fi
[ $# -ge 6 ] || { sed -n '2,15p' "$0"; exit 2; }
key=$1 version=$2 base=$3 notes=$4 out=$5
shift 5
case $base in https://*) ;; *) echo "BASE-URL must be https://" >&2; exit 1 ;; esac
case $base in */) ;; *) base="$base/" ;; esac
mkdir -p "$out"
{
	echo "zurbax-update 1"
	echo "version $version"
	echo "date $(date -u +%Y-%m-%d)"
	echo "until $(sed -n 's/^#define OS_SUPPORT_END "\(.*\)"$/\1/p' "$(dirname "$0")/../libzr/include/zr/base.h")"
	echo "base $base"
	echo "notes $notes"
	for f in "$@"; do
		echo "file $(basename "$f") $(stat -c %s "$f") $(sha256sum "$f" | cut -d' ' -f1)"
	done
} >"$out/release"
openssl dgst -sha256 -sign "$key" -out "$out/release.sig" "$out/release"
openssl dgst -sha256 -verify /dev/stdin -signature "$out/release.sig" "$out/release" >/dev/null <<KEY
$(openssl ec -in "$key" -pubout 2>/dev/null)
KEY
echo "signed $out/release:"
cat "$out/release"
