#!/bin/bash
# Regression tests for index_server, run against a live instance on the
# Haiku test VM via SSH. Each test encodes a real bug this project has
# actually hit and fixed - the point isn't broad coverage, it's making
# sure these specific regressions can't silently come back.
#
# Invoked via `./dev.sh test` (which builds, installs, and starts a fresh
# index_server first) - not meant to be run standalone, though it will
# work against any already-running instance if HAIKU_HOST/QUERY_CLIENT
# are set correctly.
set -uo pipefail

HAIKU_HOST="${HAIKU_HOST:-haiku}"
QUERY_CLIENT="${QUERY_CLIENT:?QUERY_CLIENT path not set}"
TEST_DIR="/boot/home/Desktop/index_server_tests"
RUN_ID="$(date +%s)"

pass_count=0
fail_count=0

pass() {
  pass_count=$((pass_count + 1))
  echo "PASS: $1"
}

fail() {
  fail_count=$((fail_count + 1))
  echo "FAIL: $1"
}

# query <string> - returns matching paths, one per line, on stdout.
query() {
  ssh "$HAIKU_HOST" "$QUERY_CLIENT \"$1\"" 2>/dev/null | cut -f2
}

server_alive() {
  ssh "$HAIKU_HOST" "ps | grep -q '[i]ndex_server'"
}

echo "=== index_server regression tests (run $RUN_ID) ==="

ssh "$HAIKU_HOST" "mkdir -p $TEST_DIR"

# --- Test 1: basic indexing -------------------------------------------
marker="basictest_${RUN_ID}_marker"
ssh "$HAIKU_HOST" "echo 'content $marker here' > $TEST_DIR/basic.txt"
sleep 6
if query "$marker" | grep -q "basic.txt"; then
  pass "basic indexing (new text file is searchable)"
else
  fail "basic indexing - '$marker' not found after creating basic.txt"
fi

# --- Test 2: in-place edit is live-reindexed (regression guard for #28) -
old_marker="editold_${RUN_ID}"
new_marker="editnew_${RUN_ID}"
ssh "$HAIKU_HOST" "echo 'content $old_marker here' > $TEST_DIR/edit.txt"
sleep 6
before_inode="$(ssh "$HAIKU_HOST" "ls -i $TEST_DIR/edit.txt" | awk '{print $1}')"
ssh "$HAIKU_HOST" "echo 'content $new_marker here' > $TEST_DIR/edit.txt"
after_inode="$(ssh "$HAIKU_HOST" "ls -i $TEST_DIR/edit.txt" | awk '{print $1}')"
sleep 6
if [ "$before_inode" != "$after_inode" ]; then
  fail "in-place edit test itself is broken - inode changed ($before_inode -> $after_inode), this wasn't an in-place edit"
elif query "$new_marker" | grep -q "edit.txt"; then
  pass "in-place edit live-reindex (#28 regression guard)"
else
  fail "in-place edit live-reindex - '$new_marker' not found after editing edit.txt in place (same inode $before_inode)"
fi

# --- Test 3: excluded path is not indexed -------------------------------
# The index_server settings directory itself is excluded by default -
# this is the original "feedback loop" this project's exclude list exists
# to prevent (server writes its own index -> node monitor fires -> would
# reanalyse its own index files forever without this).
excl_marker="excludetest_${RUN_ID}"
ssh "$HAIKU_HOST" "echo 'content $excl_marker here' > '/boot/home/config/settings/index_server/exclude_probe_$RUN_ID.txt'"
sleep 6
if query "$excl_marker" | grep -q "exclude_probe"; then
  fail "excluded path - '$excl_marker' WAS indexed despite living under index_server's own settings directory"
else
  pass "excluded path correctly not indexed"
fi
ssh "$HAIKU_HOST" "rm -f '/boot/home/config/settings/index_server/exclude_probe_$RUN_ID.txt'"

# --- Test 4: binary content doesn't crash the server (regression guard --
# for #27) ---------------------------------------------------------------
ssh "$HAIKU_HOST" "dd if=/dev/urandom of=$TEST_DIR/binary_$RUN_ID.bin bs=1024 count=4 2>/dev/null"
sleep 6
if server_alive; then
  pass "binary content doesn't crash index_server (#27 regression guard)"
else
  fail "index_server is not running after feeding it binary content"
fi

# --- Test 5: untyped text file is indexed directly, not via the --------
# translator path (regression guard for #27's STXTTranslator crash) -----
untyped_marker="untypedtest_${RUN_ID}"
ssh "$HAIKU_HOST" "echo 'content $untyped_marker here' > $TEST_DIR/untyped_$RUN_ID.txt
rmattr BEOS:TYPE $TEST_DIR/untyped_$RUN_ID.txt 2>/dev/null
touch $TEST_DIR/untyped_$RUN_ID.txt"
sleep 6
if ! server_alive; then
  fail "index_server crashed on an untyped text file (#27 regression)"
elif query "$untyped_marker" | grep -q "untyped_$RUN_ID.txt"; then
  pass "untyped text file indexed without crashing (#27 regression guard)"
else
  fail "untyped text file - '$untyped_marker' not found"
fi

# --- cleanup -------------------------------------------------------------
ssh "$HAIKU_HOST" "rm -rf $TEST_DIR"

echo "=== $pass_count passed, $fail_count failed ==="
[ "$fail_count" -eq 0 ]
