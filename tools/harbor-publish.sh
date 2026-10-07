#!/bin/sh
# Signs a Harbor source folder for publishing on the web.
#
#   tools/harbor-publish.sh KEY.pem SOURCE-DIR      sign SOURCE-DIR/index
#   tools/harbor-publish.sh --new-key KEY.pem       make a publisher key
#
# Keep KEY.pem private and out of this repository. The public half goes in
# data/harbor/keys/<name>.pub so ZOS trusts what you sign.
set -eu
if [ "${1:-}" = "--new-key" ]; then
	openssl ecparam -name prime256v1 -genkey -noout -out "$2"
	chmod 600 "$2"
	echo "public key (put this line in data/harbor/keys/<name>.pub):"
	openssl ec -in "$2" -pubout -outform DER 2>/dev/null | tail -c 65 | od -An -tx1 | tr -d ' \n'
	echo
	exit 0
fi
[ $# -eq 2 ] || { sed -n '2,9p' "$0"; exit 2; }
openssl dgst -sha256 -sign "$1" -out "$2/index.sig" "$2/index"
echo "signed $2/index"
