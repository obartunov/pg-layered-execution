#!/usr/bin/env python3
"""Object-level operations the consistency tests need, and nothing more.

  s3-object-admin.py stage        upload A and B under staging keys
  s3-object-admin.py head         print length and ETag of the object under test
  s3-object-admin.py swap A|B     replace the object under test, server-side
  s3-object-admin.py auth         report whether the endpoint enforces SigV4

Separate from the test script because replacement is a fault scenario here, not
a lifecycle feature: nothing in the reader creates or replaces objects.

Endpoint and credentials come from XPB_S3_* so the tests, the server and the
database all read one configuration.
"""
import os
import sys

import boto3
from botocore.config import Config
from botocore.exceptions import ClientError

# XPB_S3_UPSTREAM, not XPB_S3_ENDPOINT. When the fault proxy sits in front of
# the server the reader's endpoint is the proxy -- and the proxy re-signs, so
# it accepts anything and cannot be asked whether auth is enforced, and it
# serves only GET and HEAD so it cannot stage or replace an object. Object
# administration and the auth probe are properties of the SERVER, and both
# must address it directly.
EP = os.environ.get("XPB_S3_UPSTREAM") or os.environ.get("XPB_S3_ENDPOINT", "http://127.0.0.1:9000")
AK = os.environ.get("XPB_S3_ACCESS_KEY", "rustkey")
SK = os.environ.get("XPB_S3_SECRET_KEY", "rustsecret0123456789")
RG = os.environ.get("XPB_S3_REGION", "us-east-1")
BUCKET = os.environ.get("XPB_S3_BUCKET", "xpb")
KEY = os.environ.get("XPB_S3_KEY", "reg_buh.parquet")
A_LOCAL = os.environ.get("XPB_S3_A", "/home/claude/pg-layered-execution/"
                                     "benchmarks/02-batch-joins/data/reg_buh.parquet")
B_LOCAL = os.environ.get("XPB_S3_B", "/tmp/rustfs-run/reg_buh_B.parquet")


def client(ak=AK, sk=SK):
    return boto3.client("s3", endpoint_url=EP, aws_access_key_id=ak,
                        aws_secret_access_key=sk, region_name=RG,
                        config=Config(s3={"addressing_style": "path"},
                                      retries={"max_attempts": 1}))


def head(s3, key=KEY):
    h = s3.head_object(Bucket=BUCKET, Key=key)
    return h["ContentLength"], h["ETag"]


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "head"
    s3 = client()

    if cmd == "stage":
        try:
            s3.create_bucket(Bucket=BUCKET)
        except ClientError:
            pass
        for local, key in ((A_LOCAL, "staged_A.parquet"), (B_LOCAL, "staged_B.parquet")):
            if not os.path.exists(local):
                print(f"!! {local} absent"
                      + (" (run s3-make-object-b.py)" if key.endswith("B.parquet") else ""))
                return 2
            want = os.path.getsize(local)
            try:
                have = s3.head_object(Bucket=BUCKET, Key=key)["ContentLength"]
            except ClientError:
                have = -1
            if have != want:
                s3.upload_file(local, BUCKET, key)
            print(f"staged {key}: {s3.head_object(Bucket=BUCKET, Key=key)['ContentLength']} bytes")
        s3.copy_object(Bucket=BUCKET, Key=KEY,
                       CopySource={"Bucket": BUCKET, "Key": "staged_A.parquet"})
        n, e = head(s3)
        print(f"{KEY} = A: {n} bytes etag {e}")
        return 0

    if cmd == "head":
        n, e = head(s3)
        print(f"{n} {e}")
        return 0

    if cmd == "swap":
        which = sys.argv[2].upper()
        src = {"A": "staged_A.parquet", "B": "staged_B.parquet"}[which]
        s3.copy_object(Bucket=BUCKET, Key=KEY, CopySource={"Bucket": BUCKET, "Key": src})
        n, e = head(s3)
        print(f"{KEY} = {which}: {n} bytes etag {e}")
        return 0

    if cmd == "auth":
        # Does the endpoint actually enforce SigV4? The whole point of moving
        # off a mock that accepted unsigned requests.
        import http.client
        from urllib.parse import urlparse
        u = urlparse(EP)
        c = http.client.HTTPConnection(u.hostname, u.port or 80, timeout=30)
        c.request("GET", f"/{BUCKET}/{KEY}",
                  headers={"Host": f"{u.hostname}:{u.port}", "Range": "bytes=0-15"})
        r = c.getresponse()
        r.read()
        unsigned = r.status
        try:
            client(AK, "wrong-secret-entirely").get_object(
                Bucket=BUCKET, Key=KEY, Range="bytes=0-15")
            badsig = "ACCEPTED"
        except ClientError as e:
            badsig = e.response["Error"]["Code"]
        try:
            client("no-such-key", "whatever").get_object(
                Bucket=BUCKET, Key=KEY, Range="bytes=0-15")
            badkey = "ACCEPTED"
        except ClientError as e:
            badkey = e.response["Error"]["Code"]
        print(f"unsigned={unsigned} wrong_secret={badsig} unknown_key={badkey}")
        return 0

    print(f"!! unknown command {cmd}")
    return 2


if __name__ == "__main__":
    sys.exit(main())
