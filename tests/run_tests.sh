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
FIXTURES_DIR="${FIXTURES_DIR:-~/index_test_fixtures}"
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

# --- Test 6: non-text content is translated via the isolated helper ----
# process (RunTranslatorHelper.h) rather than in-process - this exercises
# the real Identify()+Translate() round trip through a genuine installed
# translator (RTFTranslator), not just "doesn't crash" - the translated
# text must actually come back and get indexed correctly.
rtf_marker="rtftest_${RUN_ID}"
ssh "$HAIKU_HOST" "cat > $TEST_DIR/doc_$RUN_ID.rtf << EOF
{\\\\rtf1\\\\ansi\\\\deff0
{\\\\fonttbl{\\\\f0 Times New Roman;}}
\\\\f0\\\\fs24 $rtf_marker content here.
}
EOF
mimeset $TEST_DIR/doc_$RUN_ID.rtf"
sleep 8
if ! server_alive; then
  fail "index_server crashed translating an RTF file"
elif query "$rtf_marker" | grep -q "doc_$RUN_ID.rtf"; then
  pass "RTF content translated and indexed via isolated helper"
else
  fail "translator pipeline - '$rtf_marker' not found after indexing doc_$RUN_ID.rtf"
fi

# --- Test 7: a real image is decoded and thumbnailed via the isolated --
# helper process (RunThumbnailHelper.h) - not just "doesn't crash", the
# encoded WebP thumbnail must actually come back and land in
# Media:Thumbnail the way Tracker itself reads it.
if ssh "$HAIKU_HOST" "[ -f $FIXTURES_DIR/clean_thumb_test.jpg ]"; then
  ssh "$HAIKU_HOST" "cp $FIXTURES_DIR/clean_thumb_test.jpg $TEST_DIR/thumb_$RUN_ID.jpg
  mimeset $TEST_DIR/thumb_$RUN_ID.jpg"
  sleep 10
  if ! server_alive; then
    fail "index_server crashed generating a thumbnail"
  elif ssh "$HAIKU_HOST" "catattr Media:Thumbnail $TEST_DIR/thumb_$RUN_ID.jpg 2>/dev/null" | grep -q "WEBP"; then
    pass "image thumbnail generated via isolated helper"
  else
    fail "thumbnail pipeline - no WebP Media:Thumbnail attribute on thumb_$RUN_ID.jpg"
  fi
else
  fail "thumbnail pipeline - fixture $FIXTURES_DIR/clean_thumb_test.jpg missing"
fi

# --- Test 8: a translator crashing on real (not synthetic) malformed ----
# content doesn't take index_server down with it - regression guard for
# the whole point of RunThumbnailHelper.h/RunTranslatorHelper.h. This
# fixture isn't contrived: attr_test.jpg carries an EXIF block (added for
# the AudioTagAnalyser/ExifAnalyser attribute-preservation tests) that
# reliably segfaults Haiku's own JPEGTranslator in parse_tiff_directory()
# while decoding for a thumbnail - confirmed via gdb backtrace during this
# feature's development. Before the isolation helper existed, this input
# would have crashed index_server itself.
if ssh "$HAIKU_HOST" "[ -f $FIXTURES_DIR/attr_test.jpg ]"; then
  ssh "$HAIKU_HOST" "cp $FIXTURES_DIR/attr_test.jpg $TEST_DIR/crashy_$RUN_ID.jpg
  mimeset $TEST_DIR/crashy_$RUN_ID.jpg"
  sleep 10
  if server_alive; then
    pass "translator crash on malformed EXIF doesn't take index_server down"
  else
    fail "index_server crashed - translator isolation regression"
  fi
else
  fail "translator crash guard - fixture $FIXTURES_DIR/attr_test.jpg missing"
fi

# --- Test 9: a file created empty and filled later is indexed ------------
# How "new text file" in Tracker (and most editors) works: the empty file
# is typed application/octet-stream at creation and keeps that type after
# text is written into it, so it used to stay out of the index for good.
late_marker="latefill_${RUN_ID}"
ssh "$HAIKU_HOST" ": > $TEST_DIR/late_fill.txt"
sleep 4
ssh "$HAIKU_HOST" "echo 'content $late_marker here' > $TEST_DIR/late_fill.txt"
sleep 8
if query "$late_marker" | grep -q "late_fill.txt"; then
  pass "file created empty and filled later is indexed"
else
  fail "file created empty and filled later - '$late_marker' not found (stuck on application/octet-stream?)"
fi

# --- cleanup -------------------------------------------------------------
ssh "$HAIKU_HOST" "rm -rf $TEST_DIR"

echo "=== $pass_count passed, $fail_count failed ==="
[ "$fail_count" -eq 0 ]
