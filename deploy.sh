#!/usr/bin/env bash
# Build and install Nightfall on one or more machines over SSH, from this checkout.
#
#   ./deploy.sh [--test] [--install-deps] HOST...
#   ./deploy.sh [--test] [--install-deps] --all        # hosts listed in ./deploy.hosts
#
# HOST is an ssh alias (BatchMode: keys and known host keys must already be set up), or
# "local" for the Mac this runs on. The OS is detected per host:
#   Windows  built here with mingw-w64, copied, installed into the logged-in desktop session
#   macOS    built here (universal), copied, installed to ~/Applications
#   Linux    source copied and built on the host (needs libgtk-4-dev; --install-deps runs apt)
# --test runs that platform's end-to-end suite afterwards (it blanks the screen for ~40 s).
#
# Remote files go to ~/Projects/nightfall/<platform>. deploy.hosts (one host per line,
# '#' comments) is gitignored so your machine names stay out of the repo.
set -uo pipefail
root="$(cd "$(dirname "$0")" && pwd)"
test=0; deps=0; hosts=()
for a in "$@"; do
  case "$a" in
    --test) test=1 ;;
    --install-deps) deps=1 ;;
    --all) [ -f "$root/deploy.hosts" ] || { echo "no deploy.hosts file" >&2; exit 2; }
           while read -r h _; do [[ -z "$h" || "$h" == \#* ]] || hosts+=("$h"); done < "$root/deploy.hosts" ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    *) hosts+=("$a") ;;
  esac
done
[ ${#hosts[@]} -gt 0 ] || { sed -n '2,16p' "$0"; exit 2; }

SSH=(ssh -o BatchMode=yes -o ConnectTimeout=10)
# version = latest vX.Y.Z tag in this checkout (0.0.0 before the first release)
version=$(git -C "$root" describe --tags --abbrev=0 --match 'v[0-9]*' 2>/dev/null)
version=${version#v}; version=${version:-0.0.0}
summary=()
built_win=0; built_mac=0

detect_os() {   # prints windows|macos|linux|unreachable
  local h=$1 u
  if [ "$h" = local ]; then u=$(uname -s); else u=$("${SSH[@]}" "$h" uname -s 2>/dev/null); fi
  case "$u" in
    Darwin*) echo macos; return ;;
    Linux*) echo linux; return ;;
  esac
  if "${SSH[@]}" "$h" ver 2>/dev/null | grep -qi windows; then echo windows; else echo unreachable; fi
}

build_windows() {
  [ $built_win = 1 ] && return 0
  command -v x86_64-w64-mingw32-gcc >/dev/null || { echo "need mingw-w64: brew install mingw-w64" >&2; return 1; }
  make -C "$root/windows" -s -B VERSION="$version" && built_win=1
}

build_macos() {
  [ $built_mac = 1 ] && return 0
  [ "$(uname -s)" = Darwin ] || { echo "the macOS app must be built on a Mac" >&2; return 1; }
  make -C "$root/macos" -s -B VERSION="$version" 2>&1 | grep -v 'replacing existing signature'
  [ "${PIPESTATUS[0]}" = 0 ] && built_mac=1
}

deploy_windows() {
  local h=$1 stage
  build_windows || return 1
  stage=$(mktemp -d)
  mkdir -p "$stage/windows/build" "$stage/windows/test"
  cp "$root/windows/build/nightfall.exe" "$stage/windows/build/"
  cp "$root/windows/"*.ps1 "$stage/windows/"
  cp "$root/windows/test/"*.ps1 "$stage/windows/test/"
  "${SSH[@]}" "$h" 'powershell -NoProfile -Command "New-Item -ItemType Directory -Force (Join-Path $HOME Projects\nightfall) | Out-Null"' || return 1
  scp -q -o BatchMode=yes -r "$stage/windows" "$h:Projects/nightfall/" || return 1
  rm -rf "$stage"
  "${SSH[@]}" "$h" 'powershell -NoProfile -ExecutionPolicy Bypass -File Projects\nightfall\windows\install-remote.ps1' || return 1
  if [ $test = 1 ]; then
    "${SSH[@]}" "$h" 'powershell -NoProfile -ExecutionPolicy Bypass -File Projects\nightfall\windows\test\run-e2e.ps1' | tee "/tmp/nightfall-deploy-$h.log"
  fi
}

deploy_macos() {
  local h=$1
  build_macos || return 1
  if [ "$h" = local ]; then
    "$root/macos/install.sh" || return 1
    [ $test = 1 ] && "$root/macos/test/e2e.sh" | tee "/tmp/nightfall-deploy-$h.log"
    return 0
  fi
  "${SSH[@]}" "$h" 'mkdir -p ~/Projects/nightfall/macos' || return 1
  rsync -a --delete -e "ssh -o BatchMode=yes" --exclude 'build/*.iconset' --exclude 'build/Nightfall-*' \
    "$root/macos/" "$h:Projects/nightfall/macos/" || return 1
  "${SSH[@]}" "$h" 'cd ~/Projects/nightfall/macos && codesign -v build/Nightfall.app && ./install.sh' || return 1
  [ $test = 1 ] && "${SSH[@]}" "$h" '~/Projects/nightfall/macos/test/e2e.sh' | tee "/tmp/nightfall-deploy-$h.log"
  return 0
}

deploy_linux() {
  local h=$1
  "${SSH[@]}" "$h" 'mkdir -p ~/Projects/nightfall/linux' || return 1
  rsync -a --delete -e "ssh -o BatchMode=yes" --exclude build "$root/linux/" "$h:Projects/nightfall/linux/" || return 1
  if ! "${SSH[@]}" "$h" 'pkg-config --exists gtk4 2>/dev/null'; then
    if [ $deps = 1 ]; then
      "${SSH[@]}" "$h" 'sudo -n DEBIAN_FRONTEND=noninteractive apt-get install -y -qq build-essential pkg-config libgtk-4-dev python3-evdev >/dev/null' || return 1
    else
      echo "$h: GTK4 headers missing; rerun with --install-deps (needs passwordless sudo)" >&2; return 1
    fi
  fi
  "${SSH[@]}" "$h" "cd ~/Projects/nightfall/linux && make -s -B VERSION=$version && . test/env.sh && ./install.sh" || return 1
  [ $test = 1 ] && "${SSH[@]}" "$h" '~/Projects/nightfall/linux/test/e2e.sh' | tee "/tmp/nightfall-deploy-$h.log"
  return 0
}

echo "Nightfall $version"
for h in "${hosts[@]}"; do
  echo "==> $h"
  os=$(detect_os "$h")
  if [ "$os" = unreachable ]; then
    echo "$h: unreachable (ssh failed; if its IP drifted, check the host key before trusting a new address)" >&2
    summary+=("$h unreachable"); continue
  fi
  echo "    $os"
  if "deploy_$os" "$h"; then
    status=installed
    if [ $test = 1 ]; then
      status=$(grep -E '^RESULT:' "/tmp/nightfall-deploy-$h.log" | tail -1)
      [ -n "$status" ] || status="installed, test produced no result"
    fi
  else
    status=FAILED
  fi
  summary+=("$h $os: $status")
done

echo; echo "Summary"; printf '  %s\n' "${summary[@]}"
! printf '%s\n' "${summary[@]}" | grep -qE 'FAILED|unreachable|[1-9][0-9]* failed|no result'
