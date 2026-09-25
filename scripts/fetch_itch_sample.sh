#!/usr/bin/env bash
# Downloads a full-day Nasdaq TotalView-ITCH 5.0 sample file (several GB) for
# replay, benchmarking and bootstrapping the volume baseline:
#
#   ./build/stok-replay data/itch/<file> --baseline -c config/stok.conf
#
# Nasdaq publishes sample files at https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/
# Pick any file listed there; the name encodes the date (MMDDYYYY).
set -euo pipefail
FILE="${1:-01302019.NASDAQ_ITCH50.gz}"
mkdir -p data/itch
URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/${FILE}"
echo "downloading ${URL}"
curl -fL --retry 3 -o "data/itch/${FILE}" "${URL}"
ls -lh "data/itch/${FILE}"
