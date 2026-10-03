#!/usr/bin/env bash
# Upload a zip to litterbox (72h). Prints the https URL on success.
set -euo pipefail

ZIP=${1:-}
TIME=${2:-72h}

if [[ -z "$ZIP" || ! -f "$ZIP" ]]; then
  echo "FAIL: usage: $0 <file.zip> [1h|12h|24h|72h]" >&2
  exit 2
fi

case "$TIME" in
  1h|12h|24h|72h) ;;
  *) echo "FAIL: time must be 1h|12h|24h|72h" >&2; exit 2 ;;
esac

API=https://litterbox.catbox.moe/resources/internals/api.php
sleep_s=4
url=""
for attempt in 1 2 3 4; do
  if url=$(curl -fsS --max-time 180 \
      -F "reqtype=fileupload" \
      -F "time=${TIME}" \
      -F "fileToUpload=@${ZIP}" \
      "$API"); then
    if [[ "$url" == https://litter.catbox.moe/* ]]; then
      echo "$url"
      echo "LITTERBOX OK (${TIME})"
      exit 0
    fi
    echo "FAIL: unexpected response: $url" >&2
  else
    echo "FAIL: curl attempt ${attempt}" >&2
  fi
  if [[ "$attempt" -lt 4 ]]; then
    sleep "$sleep_s"
    sleep_s=$((sleep_s * 2))
  fi
done

echo "FAIL: litterbox upload failed after 4 attempts" >&2
exit 1
