#!/bin/bash
#
# Bring up a local S3-compatible endpoint and put the benchmark Parquet file in
# it. moto, not AWS: there is no route to AWS from this environment, and a test
# that needs one would never run.
#
#   extension/xpb_parquet/test/s3-mock-setup.sh [PORT]
#
# What this is NOT: evidence that the reader works against real S3. moto
# accepts unsigned requests, so it cannot check the signature; SigV4 is checked
# against botocore instead (sigv4_*). See docs/S3_READER_PHASE1.md.
set -uo pipefail
PORT="${1:-5555}"
PROXY_PORT="${2:-5556}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PQ="$REPO/benchmarks/02-batch-joins/data/reg_buh.parquet"
RUN="${TMPDIR:-/tmp}/xpb_s3_mock"

[ -f "$PQ" ] || { echo "!! $PQ absent (run benchmarks/02-batch-joins/export-parquet.py)"; exit 2; }
python3 -c "import moto, boto3" 2>/dev/null || {
    echo "!! needs: pip install --break-system-packages 'moto[server]' boto3"; exit 2; }

mkdir -p "$RUN"
if ! curl -fsS -o /dev/null "http://127.0.0.1:$PORT/" 2>/dev/null; then
    echo "starting moto on 127.0.0.1:$PORT"
    nohup python3 -m moto.server -p "$PORT" -H 127.0.0.1 > "$RUN/moto.log" 2>&1 &
    for _ in $(seq 1 40); do
        curl -fsS -o /dev/null "http://127.0.0.1:$PORT/" 2>/dev/null && break
        sleep 0.25
    done
fi
curl -fsS -o /dev/null "http://127.0.0.1:$PORT/" || { echo "!! moto did not come up"; exit 2; }

python3 - "$PORT" "$PQ" <<'PY'
import sys, boto3, os
port, pq = sys.argv[1], sys.argv[2]
s3 = boto3.client("s3", endpoint_url=f"http://127.0.0.1:{port}",
                  aws_access_key_id="testkey", aws_secret_access_key="testsecret",
                  region_name="us-east-1")
try: s3.create_bucket(Bucket="xpb")
except Exception: pass
want = os.path.getsize(pq)
try:
    have = s3.head_object(Bucket="xpb", Key="reg_buh.parquet")["ContentLength"]
except Exception:
    have = -1
if have != want:
    s3.upload_file(pq, "xpb", "reg_buh.parquet")
    have = s3.head_object(Bucket="xpb", Key="reg_buh.parquet")["ContentLength"]
print(f"s3://xpb/reg_buh.parquet  {have} bytes (local {want})")
assert have == want, "upload length mismatch"
PY

# The fault proxy, phase 2 onward. Idempotent like the rest of this script:
# start it only if nothing is listening, so a test can call this freely.
CONTROL="${XPB_S3_CONTROL:-/tmp/xpbs3/mode}"
mkdir -p "$(dirname "$CONTROL")"; chmod 777 "$(dirname "$CONTROL")" 2>/dev/null
[ -f "$CONTROL" ] || { echo ok > "$CONTROL"; }
chmod 666 "$CONTROL" 2>/dev/null

if ! curl -fsS -o /dev/null "http://127.0.0.1:$PROXY_PORT/xpb/reg_buh.parquet" -r 0-1 2>/dev/null; then
    echo "starting fault proxy on 127.0.0.1:$PROXY_PORT"
    setsid --fork python3 "$(dirname "${BASH_SOURCE[0]}")/s3-fault-proxy.py" \
        --port "$PROXY_PORT" --upstream "127.0.0.1:$PORT" --control "$CONTROL" \
        > "$RUN/proxy.log" 2>&1 < /dev/null
    for _ in $(seq 1 40); do
        curl -fsS -o /dev/null "http://127.0.0.1:$PROXY_PORT/xpb/reg_buh.parquet" -r 0-1 2>/dev/null && break
        sleep 0.25
    done
fi
curl -fsS -o /dev/null "http://127.0.0.1:$PROXY_PORT/xpb/reg_buh.parquet" -r 0-1 2>/dev/null \
    && echo "fault proxy on 127.0.0.1:$PROXY_PORT -> 127.0.0.1:$PORT, control $CONTROL" \
    || echo "!! the fault proxy did not come up; phase 2+ sections will skip"

cat <<ENV

The server must be started with these in ITS environment -- a backend is forked
from the postmaster and does not inherit a client's:

  XPB_S3_ENDPOINT=http://127.0.0.1:$PROXY_PORT   (through the fault proxy)
  XPB_S3_CONTROL=$CONTROL
  XPB_S3_ACCESS_KEY=testkey
  XPB_S3_SECRET_KEY=testsecret
  XPB_S3_REGION=us-east-1
ENV
