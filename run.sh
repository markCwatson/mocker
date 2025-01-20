#!/bin/sh

set -e
tmpFile=$(mktemp)
gcc -lcurl src/*.c -o $tmpFile
exec "$tmpFile" "$@"
