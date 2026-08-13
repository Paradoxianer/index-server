#!/bin/bash
# Dev cycle for the standalone index_server project: sync -> Makefile-Engine
# build -> install -> run -> collect log. Builds against a normal Haiku
# devel install (clucene_devel, taglib_devel, libexif_devel, plus the
# system's own private headers/libcolumnlistview.a) - no full Haiku source
# tree needed.
set -euo pipefail

HAIKU_HOST="haiku"
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

TARGETS="server add-ons/fulltext add-ons/audiotags add-ons/exif add-ons/mediakit add-ons/mail add-ons/thumbnail preferences search-app"
BINARY_NAMES="index_server FullTextAnalyser AudioTagAnalyser ExifAnalyser MediaKitAnalyser MailAnalyser ThumbnailAnalyser IndexServerSettings IndexServerSearch"

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
      (cd \$target && make) >> build.log 2>&1; \
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
package() {
  build
  ssh "$HAIKU_HOST" "rm -rf $PKG_STAGE_DIR && mkdir -p \
      $PKG_STAGE_DIR/data/launch \
      $PKG_STAGE_DIR/data/deskbar/menu/Applications \
      $PKG_STAGE_DIR/data/deskbar/menu/Preferences \
      $PKG_STAGE_DIR/servers \
      $PKG_STAGE_DIR/add-ons/index_server \
      $PKG_STAGE_DIR/preferences \
      $PKG_STAGE_DIR/apps && \
    cp $REMOTE_TREE/server/data/launch/index_server \
      $PKG_STAGE_DIR/data/launch/ && \
    cp -P $REMOTE_TREE/server/data/deskbar/menu/Applications/IndexServerSearch \
      $PKG_STAGE_DIR/data/deskbar/menu/Applications/ && \
    cp -P $REMOTE_TREE/server/data/deskbar/menu/Preferences/IndexServerSettings \
      $PKG_STAGE_DIR/data/deskbar/menu/Preferences/ && \
    cp $(_robjdir server)/index_server $PKG_STAGE_DIR/servers/ && \
    cp $(_robjdir add-ons/fulltext)/FullTextAnalyser \
      $(_robjdir add-ons/audiotags)/AudioTagAnalyser \
      $(_robjdir add-ons/exif)/ExifAnalyser \
      $(_robjdir add-ons/mediakit)/MediaKitAnalyser \
      $(_robjdir add-ons/mail)/MailAnalyser \
      $(_robjdir add-ons/thumbnail)/ThumbnailAnalyser \
      $PKG_STAGE_DIR/add-ons/index_server/ && \
    cp $(_robjdir preferences)/IndexServerSettings $PKG_STAGE_DIR/preferences/ && \
    cp $(_robjdir search-app)/IndexServerSearch $PKG_STAGE_DIR/apps/ && \
    rm -f ~/index_server.hpkg && \
    cd $PKG_STAGE_DIR && \
    package create -i $REMOTE_TREE/server/.PackageInfo -C . ~/index_server.hpkg"
  scp "$HAIKU_HOST:~/index_server.hpkg" ./index_server.hpkg
}

package_install() {
  scp ./index_server.hpkg "$HAIKU_HOST:~/index_server.hpkg"
  ssh "$HAIKU_HOST" "pkgman install -y ~/index_server.hpkg"
}

package_uninstall() {
  ssh "$HAIKU_HOST" "pkgman uninstall -y index_server"
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
  *) echo "usage: $0 {bootstrap|sync|build|install|run|build-and-run|start-logged|debug-log|stop|status|debug-report|package|package-install|package-uninstall}"; exit 1 ;;
esac
