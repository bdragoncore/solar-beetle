#!/usr/bin/env bash
# Sign a firmware .bin file with RSA-2048 private key
# Usage: ./sign_firmware.sh <firmware.bin> [output.signed.bin]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KEY="$SCRIPT_DIR/../keys/private.pem"

if [ $# -lt 1 ]; then
  echo "Usage: $0 <firmware.bin> [output.signed.bin]"
  exit 1
fi

INPUT="$1"
OUTPUT="${2:-${INPUT%.bin}.signed.bin}"

if [ ! -f "$KEY" ]; then
  echo "Error: private key not found at $KEY"
  exit 1
fi

# Compute SHA-256 hash and sign with RSA-2048 PKCS#1 v1.5
SIGNATURE=$(openssl dgst -sha256 -sign "$KEY" "$INPUT" | xxd -p)

# Append 256-byte signature to firmware
cp "$INPUT" "$OUTPUT"
echo -n "$SIGNATURE" | xxd -r -p >> "$OUTPUT"

echo "Signed firmware: $(basename "$INPUT") -> $(basename "$OUTPUT")"
echo "Size: firmware=$(stat -c%s "$INPUT") bytes, signature=256 bytes"
