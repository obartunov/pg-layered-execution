import sys
from botocore.auth import SigV4Auth
from botocore.awsrequest import AWSRequest
from botocore.credentials import Credentials
method, uri, host, rng, amz_date = sys.argv[1:6]
headers = {"host": host,
           "x-amz-content-sha256":
               "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
           "x-amz-date": amz_date}
if rng: headers["range"] = rng
req = AWSRequest(method=method, url="http://" + host + uri, headers=headers)
# canonical_request() reads no clock, so nothing needs freezing here.
auth = SigV4Auth(Credentials("k", "s"), "s3", "us-east-1")
sys.stdout.write(auth.canonical_request(req))
