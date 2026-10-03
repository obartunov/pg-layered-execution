#!/bin/bash
#
# Restart the test postmaster with the environment the remote arm needs.
#
# The S3 reader takes its endpoint and credentials from the environment of the
# BACKEND process, which is the postmaster's environment, so a restart that
# loses them turns the remote arm into an error rather than a slow arm. Listed
# here explicitly for that reason, and because a cold run whose configuration
# differs from the warm run is not a comparison.
set -e
export XPB_S3_ENDPOINT="${XPB_S3_ENDPOINT:-http://127.0.0.1:9000}"
export XPB_S3_UPSTREAM="${XPB_S3_UPSTREAM:-http://127.0.0.1:9000}"
export XPB_S3_ACCESS_KEY="${XPB_S3_ACCESS_KEY:-rustkey}"
export XPB_S3_SECRET_KEY="${XPB_S3_SECRET_KEY:-rustsecret0123456789}"
export XPB_S3_REGION="${XPB_S3_REGION:-us-east-1}"
export XPB_S3_MAX_ATTEMPTS="${XPB_S3_MAX_ATTEMPTS:-3}"
export XPB_S3_IO_TIMEOUT_MS="${XPB_S3_IO_TIMEOUT_MS:-15000}"
PGDATA="${PGDATA:-/tmp/pgdata20}"
PGBIN="${PGBIN:-/home/claude/pginstall20/bin}"
su pguser -c "env XPB_S3_ENDPOINT=$XPB_S3_ENDPOINT XPB_S3_UPSTREAM=$XPB_S3_UPSTREAM \
  XPB_S3_ACCESS_KEY=$XPB_S3_ACCESS_KEY XPB_S3_SECRET_KEY=$XPB_S3_SECRET_KEY \
  XPB_S3_REGION=$XPB_S3_REGION XPB_S3_MAX_ATTEMPTS=$XPB_S3_MAX_ATTEMPTS \
  XPB_S3_IO_TIMEOUT_MS=$XPB_S3_IO_TIMEOUT_MS \
  $PGBIN/pg_ctl -D $PGDATA -l $PGDATA/server.log restart -w -t 60"
