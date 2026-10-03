import sys, datetime as _dt
import botocore.auth as A
from botocore.auth import SigV4Auth
from botocore.awsrequest import AWSRequest
from botocore.credentials import Credentials

method, uri, host, rng, amz_date, datestamp, region, access, secret = sys.argv[1:10]
FROZEN = _dt.datetime.strptime(amz_date, "%Y%m%dT%H%M%SZ").replace(tzinfo=_dt.timezone.utc)
# add_auth() takes the timestamp from this module-level helper, and both the
# credential scope and the X-Amz-Date header derive from it. Patching
# _get_date, request.context or the datetime module does not reach it.
A.get_current_datetime = lambda: FROZEN

headers = {"host": host,
           "x-amz-content-sha256":
               "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
           "x-amz-date": amz_date}
if rng: headers["range"] = rng
req = AWSRequest(method=method, url="http://" + host + uri, headers=headers)
auth = SigV4Auth(Credentials(access, secret), "s3", region)
auth.add_auth(req)
print(req.headers["Authorization"])
