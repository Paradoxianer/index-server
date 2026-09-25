#!/bin/bash
# Dev cycle for the standalone index_server project: sync -> Makefile-Engine
# build -> install -> run -> collect log. Builds against a normal Haiku
# devel install (clucene_devel, taglib_devel, libexif_devel, plus the
# system's own private headers/libcolumnlistview.a) - no full Haiku source
# tree needed.
set -euo pipefail

HAIKU_HOST="${HAIKU_HOST:-haiku}"
REMOTE_TREE="~/repos/index-server"
LOCAL_TREE="$HOME/repos/index-server"
ADDON_DIR="/boot/system/non-packaged/add-ons/index_server"
SERVER_DIR="/boot/system/non-packaged/servers"
PREFLET_DIR="/boot/system/non-packaged/preferences"
APPS_DIR="/boot/system/non-packaged/apps"
SERVER_LOG="server.log"
TEST_DIR="~/index_test"
FIXTURES_DIR="~/index_test_fixtures"
SETTINGS_DIR="~/config/settings/index_server"
SETTINGS_FILE="$SETTINGS_DIR/settings"
DEBUG_LOG="~/index_server_debug.log"
DEVEL_PACKAGES="clucene_devel taglib_devel libexif_devel"
MAKE="make"
PKG_NAME="index_server"
PKG_ADDONS_SUBDIR="add-ons/index_server"

# 32-bit Haiku: build with the secondary x86 (gcc 13) toolchain and its
# devel packages, e.g. HAIKU_HOST=haiku32 HAIKU_ARCH=x86 ./dev.sh build
if [ "${HAIKU_ARCH:-}" = "x86" ]; then
  MAKE="setarch x86 make"
  DEVEL_PACKAGES="clucene_x86_devel taglib_x86_devel libexif_x86_devel"
  # A secondary-architecture process only looks in add-ons/x86/.
  ADDON_DIR="/boot/system/non-packaged/add-ons/x86/index_server"
  PKG_NAME="index_server_x86"
  PKG_ADDONS_SUBDIR="add-ons/x86/index_server"
fi

TARGETS="server translate-helper thumbnail-helper add-ons/fulltext add-ons/audiotags add-ons/exif add-ons/mediakit add-ons/mail add-ons/thumbnail preferences search-app tests"
BINARY_NAMES="index_server IndexServerTranslateHelper IndexServerThumbnailHelper FullTextAnalyser AudioTagAnalyser ExifAnalyser MediaKitAnalyser MailAnalyser ThumbnailAnalyser IndexServerSettings IndexServerSearch QueryClient"

cmd="${1:-build}"

# One-time setup on a fresh VM: clone the repo and install the devel
# packages the Makefiles link/compile against.
bootstrap() {
  ssh "$HAIKU_HOST" "mkdir -p ~/repos && [ -d $REMOTE_TREE ] || git clone https://github.com/Paradoxianer/index-server.git $REMOTE_TREE"
  ssh "$HAIKU_HOST" "pkgman install -y $DEVEL_PACKAGES"
}

sync() {
  rsync -avz --delete --exclude .git "$LOCAL_TREE/" "$HAIKU_HOST:$REMOTE_TREE/"
}

# Finds the Makefile-Engine's objects.* output directory for a target - its
# exact name (compiler/arch/optimization-qualified) isn't fixed, so glob for
# it rather than hardcoding.
_objdir() {
  ssh "$HAIKU_HOST" "ls -d $REMOTE_TREE/$1/objects.* 2>/dev/null | head -1"
}

build() {
  sync
  ssh "$HAIKU_HOST" "cd $REMOTE_TREE && rm -f build.log && \
    for target in $TARGETS; do \
      echo \"=== \$target ===\" >> build.log; \
      (cd \$target && $MAKE) >> build.log 2>&1; \
    done; \
    cat build.log"
  scp "$HAIKU_HOST:$REMOTE_TREE/build.log" ./build.log
}

install() {
  # Atomic replace (temp name + rename) so a live index_server's own
  # AddOnMonitorHandler doesn't race a straight overwrite of an in-use
  # add-on/binary - see the project's history (#44) for why this matters.
  ssh "$HAIKU_HOST" "mkdir -p $SERVER_DIR $ADDON_DIR $PREFLET_DIR $APPS_DIR && \
    install_one() { cp \"\$1\" \"\$2.new\" && mv \"\$2.new\" \"\$2\"; }; \
    install_one $(_objdir server)/index_server $SERVER_DIR/index_server && \
    install_one $(_objdir translate-helper)/IndexServerTranslateHelper $SERVER_DIR/IndexServerTranslateHelper && \
    install_one $(_objdir thumbnail-helper)/IndexServerThumbnailHelper $SERVER_DIR/IndexServerThumbnailHelper && \
    install_one $(_objdir add-ons/fulltext)/FullTextAnalyser $ADDON_DIR/FullTextAnalyser && \
    install_one $(_objdir add-ons/audiotags)/AudioTagAnalyser $ADDON_DIR/AudioTagAnalyser && \
    install_one $(_objdir add-ons/exif)/ExifAnalyser $ADDON_DIR/ExifAnalyser && \
    install_one $(_objdir add-ons/mediakit)/MediaKitAnalyser $ADDON_DIR/MediaKitAnalyser && \
    install_one $(_objdir add-ons/mail)/MailAnalyser $ADDON_DIR/MailAnalyser && \
    install_one $(_objdir add-ons/thumbnail)/ThumbnailAnalyser $ADDON_DIR/ThumbnailAnalyser && \
    install_one $(_objdir preferences)/IndexServerSettings $PREFLET_DIR/IndexServerSettings && \
    install_one $(_objdir search-app)/IndexServerSearch $APPS_DIR/IndexServerSearch"
}

stop() {
  ssh "$HAIKU_HOST" "pid=\$(ps | awk '\$1 == \"$SERVER_DIR/index_server\" {print \$2}'); \
    if [ -z \"\$pid\" ]; then echo 'index_server: not running'; \
    else \
      kill \$pid; \
      for i in \$(seq 1 20); do \
        kill -0 \$pid 2>/dev/null || break; \
        sleep 0.5; \
      done; \
      kill -9 \$pid 2>/dev/null; \
    fi; \
    rm -f $SETTINGS_DIR/FullTextAnalyser/index_server.lock $SETTINGS_DIR/FullTextAnalyser/index_server_dircreate.lock"
}

run() {
  stop
  install
  ssh "$HAIKU_HOST" "rm -f ~/$SERVER_LOG $SETTINGS_FILE && rm -rf $TEST_DIR && mkdir -p $TEST_DIR && \
    ($SERVER_DIR/index_server > ~/$SERVER_LOG 2>&1 &) && \
    sleep 3 && \
    echo 'The quick haiku probe fox jumps over the lazy translator' \
      > $TEST_DIR/probe.txt && \
    cp $FIXTURES_DIR/* $TEST_DIR/ 2>/dev/null; \
    mimeset $TEST_DIR/* 2>/dev/null; \
    sleep 60"
  stop
  scp "$HAIKU_HOST:~/$SERVER_LOG" "./$SERVER_LOG"
}

start_logged() {
  stop
  install
  ssh "$HAIKU_HOST" "rm -f $DEBUG_LOG && ($SERVER_DIR/index_server > $DEBUG_LOG 2>&1 &)"
}

debug_log() {
  scp "$HAIKU_HOST:$DEBUG_LOG" ./index_server_debug.log
}

status() {
  ssh "$HAIKU_HOST" "ps | grep '$SERVER_DIR/index_server' || echo 'index_server: not running'; \
    echo '--- log (live) ---'; \
    cat ~/$SERVER_LOG 2>&1"
}

# Finds the Makefile-Engine objects.* dir for a target, remote path form
# (for use inside an already-open ssh command, unlike _objdir).
_robjdir() {
  echo "\$(ls -d $REMOTE_TREE/$1/objects.* 2>/dev/null | head -1)"
}

# Builds a real, installable index_server.hpkg from the current build
# artifacts and copies it to ./index_server.hpkg locally. Localization
# catalogs aren't wired into the Makefile-Engine build yet (the existing
# .catkeys files were generated by Jam's DoCatalogs, which uses a signature
# convention linkcatkeys here doesn't accept as-is) - ships English-only
# for now.
PKG_STAGE_DIR="~/index_server_pkg_stage"
# The package description for the current architecture. The secondary
# architecture package is named index_server_x86, has architecture
# x86_gcc2 (what x86_gcc2h hybrid images call their secondary packages)
# and depends on the _x86 flavours of its dependencies.
_package_info() {
  if [ "${HAIKU_ARCH:-}" = "x86" ]; then
    sed -E \
      -e 's/^(name[[:space:]]+)index_server$/\1index_server_x86/' \
      -e 's/^(architecture[[:space:]]+)x86_64$/\1x86_gcc2/' \
      -e 's/^([[:space:]]+)index_server = /\1index_server_x86 = /' \
      -e 's/^([[:space:]]+)(haiku|clucene|taglib|libexif) >=/\1\2_x86 >=/' \
      "$LOCAL_TREE/server/.PackageInfo"
  else
    cat "$LOCAL_TREE/server/.PackageInfo"
  fi
}

package() {
  build
  _package_info > "./$PKG_NAME.PackageInfo"
  scp "./$PKG_NAME.PackageInfo" "$HAIKU_HOST:~/$PKG_NAME.PackageInfo"
  rm -f "./$PKG_NAME.PackageInfo"
  ssh "$HAIKU_HOST" "rm -rf $PKG_STAGE_DIR && mkdir -p \
      $PKG_STAGE_DIR/data/launch \
      $PKG_STAGE_DIR/data/deskbar/menu/Applications \
      $PKG_STAGE_DIR/data/deskbar/menu/Preferences \
      $PKG_STAGE_DIR/servers \
      $PKG_STAGE_DIR/$PKG_ADDONS_SUBDIR \
      $PKG_STAGE_DIR/preferences \
      $PKG_STAGE_DIR/apps && \
    cp $REMOTE_TREE/server/data/launch/index_server \
      $PKG_STAGE_DIR/data/launch/ && \
    cp -P $REMOTE_TREE/server/data/deskbar/menu/Applications/IndexServerSearch \
      $PKG_STAGE_DIR/data/deskbar/menu/Applications/ && \
    cp -P $REMOTE_TREE/server/data/deskbar/menu/Preferences/IndexServerSettings \
      $PKG_STAGE_DIR/data/deskbar/menu/Preferences/ && \
    cp $(_robjdir server)/index_server \
      $(_robjdir translate-helper)/IndexServerTranslateHelper \
      $(_robjdir thumbnail-helper)/IndexServerThumbnailHelper \
      $PKG_STAGE_DIR/servers/ && \
    cp $(_robjdir add-ons/fulltext)/FullTextAnalyser \
      $(_robjdir add-ons/audiotags)/AudioTagAnalyser \
      $(_robjdir add-ons/exif)/ExifAnalyser \
      $(_robjdir add-ons/mediakit)/MediaKitAnalyser \
      $(_robjdir add-ons/mail)/MailAnalyser \
      $(_robjdir add-ons/thumbnail)/ThumbnailAnalyser \
      $PKG_STAGE_DIR/$PKG_ADDONS_SUBDIR/ && \
    cp $(_robjdir preferences)/IndexServerSettings $PKG_STAGE_DIR/preferences/ && \
    cp $(_robjdir search-app)/IndexServerSearch $PKG_STAGE_DIR/apps/ && \
    rm -f ~/$PKG_NAME.hpkg && \
    cd $PKG_STAGE_DIR && \
    package create -i ~/$PKG_NAME.PackageInfo -C . ~/$PKG_NAME.hpkg"
  scp "$HAIKU_HOST:~/$PKG_NAME.hpkg" "./$PKG_NAME.hpkg"
}

package_install() {
  scp "./$PKG_NAME.hpkg" "$HAIKU_HOST:~/$PKG_NAME.hpkg"
  ssh "$HAIKU_HOST" "pkgman install -y ~/$PKG_NAME.hpkg"
}

package_uninstall() {
  ssh "$HAIKU_HOST" "pkgman uninstall -y $PKG_NAME"
}

# Builds, starts a clean index_server, and runs tests/run_tests.sh against
# it via QueryClient (see tests/QueryClient.cpp for why a dedicated CLI
# client is used instead of hey/GUI scripting). Leaves the server running
# afterwards so a failure can be investigated with `./dev.sh status`.
regression_test() {
  build
  stop
  install
  ssh "$HAIKU_HOST" "rm -f ~/$SERVER_LOG $SETTINGS_FILE && \
    ($SERVER_DIR/index_server > ~/$SERVER_LOG 2>&1 &) && sleep 3"
  QUERY_CLIENT="$(_objdir tests)/QueryClient" HAIKU_HOST="$HAIKU_HOST" \
    FIXTURES_DIR="$FIXTURES_DIR" bash "$LOCAL_TREE/tests/run_tests.sh"
}

debug_report() {
  remote_path=$(ssh "$HAIKU_HOST" "ls -t ~/Desktop/*.report 2>/dev/null | head -1")
  if [ -z "$remote_path" ]; then
    echo "no debug report found in ~/Desktop on $HAIKU_HOST"
    return 1
  fi
  scp "$HAIKU_HOST:$remote_path" ./debug_report.txt
}

case "$cmd" in
  bootstrap) bootstrap ;;
  sync) sync ;;
  build) build ;;
  install) install ;;
  run) run ;;
  build-and-run) build && run ;;
  start-logged) start_logged ;;
  debug-log) debug_log ;;
  stop) stop ;;
  status) status ;;
  debug-report) debug_report ;;
  package) package ;;
  package-install) package_install ;;
  package-uninstall) package_uninstall ;;
  test) regression_test ;;
  *) echo "usage: $0 {bootstrap|sync|build|install|run|build-and-run|start-logged|debug-log|stop|status|debug-report|package|package-install|package-uninstall|test}"; exit 1 ;;
esac
