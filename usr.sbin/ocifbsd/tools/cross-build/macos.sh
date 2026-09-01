#!/bin/sh
# macos.sh - Set up a clean macOS host for ocifbsd cross-build
#
# This script automates the toolchain setup so the cross-build process
# is repeatable on a fresh macOS install. It is idempotent: safe to
# run multiple times.
#
# Usage:
#   ./macos.sh              # Check + setup env (no installs)
#   ./macos.sh --install    # Auto-install ALL missing tools
#   ./macos.sh --check      # Just check, don't set env
#   ./macos.sh --yes        # Skip confirmations
#
# After running, source the generated env file:
#   . /tmp/ocifbsd-cross-build-env
#
# Then build:
#   make cross-build        # from the repo root, OR
#   bmake -C usr.sbin/ocifbsd TARGET=amd64 TARGET_ARCH=amd64
#
# Tested on a clean macOS install: Xcode CLT -> brew -> bmake -> llvm.
# Each step is skipped if already present.
#
# === IMPORTANT ===
# This is a CROSS-BUILD helper. The primary, tier-1 build target is
# FreeBSD native (`bmake -C usr.sbin/ocifbsd` on a FreeBSD host).
# This script exists only so that developers on macOS workstations
# can cross-build to FreeBSD without needing a FreeBSD VM for the
# compile step. See tools/cross-build/README.md.

set -eu

# === Configuration ===
BREW_INSTALL_URL="https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh"
ENV_FILE="${OCIFBSD_ENV_FILE:-/tmp/ocifbsd-cross-build-env}"
OBJ_DIR="${OCIFBSD_OBJ_DIR:-/tmp/obj-oci}"
TARGET="${TARGET:-amd64}"
TARGET_ARCH="${TARGET_ARCH:-amd64}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

# === Colors ===
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log()  { printf "${GREEN}[+]${NC} %s\n" "$1"; }
warn() { printf "${YELLOW}[!]${NC} %s\n" "$1" >&2; }
err()  { printf "${RED}[X]${NC} %s\n" "$1" >&2; }
hdr()  { printf "\n${BLUE}== %s ==${NC}\n" "$1"; }
ok()   { printf "${GREEN}    OK${NC}\n"; }

# === Mode parsing ===
MODE="setup"
ASSUME_YES=false
for arg in "$@"; do
    case "$arg" in
        --install) MODE="install" ;;
        --check)   MODE="check"   ;;
        --yes|-y)  ASSUME_YES=true ;;
        --help|-h)
            sed -n '2,25p' "$0"
            exit 0
            ;;
        *)
            err "Unknown argument: $arg (use --help)"
            exit 2
            ;;
    esac
done

# === Helpers ===
confirm() {
    if $ASSUME_YES; then return 0; fi
    printf "%s [y/N] " "$1"
    read -r ans
    case "$ans" in
        y|Y|yes|YES) return 0 ;;
        *) return 1 ;;
    esac
}

is_macos() {
    [ "$(uname -s)" = "Darwin" ]
}

brew_prefix() {
    if is_macos && [ "$(uname -m)" = "arm64" ]; then
        echo "/opt/homebrew"
    else
        echo "/usr/local"
    fi
}

# === Pre-flight ===
if ! is_macos; then
    err "This script is for macOS only. Detected: $(uname -s)"
    exit 1
fi

BREW_PREFIX="$(brew_prefix)"
LLVM_PREFIX="${BREW_PREFIX}/opt/llvm"
BMAKE="${BREW_PREFIX}/bin/bmake"

hdr "ocifbsd macOS bootstrap"
echo "Brew prefix: $BREW_PREFIX"
echo "LLVM prefix: $LLVM_PREFIX"
echo "Target:      $TARGET / $TARGET_ARCH"
echo "Jobs:        $JOBS"
echo "Mode:        $MODE"
echo "Env file:    $ENV_FILE"
echo "User:        $(whoami) (sudo: $(sudo -n true 2>/dev/null && echo yes || echo no))"

# === Step 1: Xcode Command Line Tools ===
hdr "Step 1/5: Xcode Command Line Tools"
if xcode-select -p >/dev/null 2>&1; then
    log "Xcode CLT: $(xcode-select -p)"
    ok
else
    warn "Xcode Command Line Tools NOT installed"
    if [ "$MODE" = "install" ]; then
        log "Triggering Xcode CLT install (a dialog will appear)..."
        xcode-select --install || true
        log "Please complete the Xcode CLT installation dialog."
        log "After it finishes, re-run this script to continue."
        exit 0
    else
        err "Xcode CLT required. Install with: xcode-select --install"
        exit 1
    fi
fi

# === Step 2: Homebrew ===
hdr "Step 2/5: Homebrew"
if command -v brew >/dev/null 2>&1; then
    BREW_PREFIX="$(brew --prefix)"
    LLVM_PREFIX="${BREW_PREFIX}/opt/llvm"
    BMAKE="${BREW_PREFIX}/bin/bmake"
    log "Homebrew: $(brew --version | head -1)"
    log "Prefix:   $BREW_PREFIX"
    ok
else
    warn "Homebrew NOT installed"
    if [ "$MODE" = "install" ]; then
        if confirm "Install Homebrew now? (requires sudo)"; then
            log "Installing Homebrew from $BREW_INSTALL_URL"
            log "You will be prompted for your password..."
            NONINTERACTIVE=0 /bin/bash -c "$(curl -fsSL "$BREW_INSTALL_URL")"
            # After install, brew is available but PATH may need update
            if [ -x "${BREW_PREFIX}/bin/brew" ]; then
                eval "$(${BREW_PREFIX}/bin/brew shellenv)"
            fi
            BREW_PREFIX="$(brew --prefix)"
            LLVM_PREFIX="${BREW_PREFIX}/opt/llvm"
            BMAKE="${BREW_PREFIX}/bin/bmake"
            log "Homebrew installed: $(brew --version | head -1)"
            ok
        else
            err "Homebrew required. Install manually from https://brew.sh"
            exit 1
        fi
    else
        err "Homebrew required. Install from https://brew.sh or re-run with --install"
        exit 1
    fi
fi

# === Step 3: bmake ===
hdr "Step 3/5: bmake (FreeBSD make)"
if command -v bmake >/dev/null 2>&1; then
    log "bmake: $(bmake --version 2>&1 | head -1)"
    ok
elif [ -x "$BMAKE" ]; then
    log "bmake: $BMAKE"
    ok
else
    warn "bmake NOT installed"
    if [ "$MODE" = "install" ]; then
        log "Installing bmake via brew..."
        brew install bmake
        log "bmake: installed"
        ok
    else
        err "bmake required. Install with: brew install bmake"
        exit 1
    fi
fi

# === Step 4: LLVM toolchain ===
hdr "Step 4/5: LLVM toolchain (clang, clang++, lld)"
# In LLVM 19+, lld was split into a separate brew package.
# So we need to check both $LLVM_PREFIX/bin/ and $BREW_PREFIX/bin/ for lld.
#
# Only clang/clang++ (compile) and lld (link) are required to cross-build.
# lldb is a debugger, not part of the build; some llvm bottles (e.g. on
# pre-release macOS) omit it, and Xcode CLT ships one anyway, so it is
# checked warn-only below and never fails the setup.
LLVM_OK=true
for tool in clang clang++; do
    if [ -x "$LLVM_PREFIX/bin/$tool" ]; then
        log "$tool: $("$LLVM_PREFIX/bin/$tool" --version 2>&1 | head -1)"
    else
        warn "$tool: NOT found at $LLVM_PREFIX/bin/$tool"
        LLVM_OK=false
    fi
done
# lldb (optional, debug-only): prefer the llvm one, fall back to Xcode CLT.
if [ -x "$LLVM_PREFIX/bin/lldb" ]; then
    log "lldb: $("$LLVM_PREFIX/bin/lldb" --version 2>&1 | head -1) (optional)"
elif xcrun -f lldb >/dev/null 2>&1; then
    log "lldb: $(xcrun -f lldb) (optional, from Xcode CLT)"
else
    warn "lldb: not found (optional; debugging only, not needed to build)"
fi
# lld: check both locations
if [ -x "$LLVM_PREFIX/bin/lld" ]; then
    log "lld: $("$LLVM_PREFIX/bin/lld" --version 2>&1 | head -1) (in llvm prefix)"
elif [ -x "$BREW_PREFIX/bin/lld" ]; then
    log "lld: $("$BREW_PREFIX/bin/lld" --version 2>&1 | head -1) (in brew prefix, separate package)"
else
    warn "lld: NOT found at $LLVM_PREFIX/bin/lld or $BREW_PREFIX/bin/lld"
    LLVM_OK=false
fi

if $LLVM_OK; then
    ok
else
    warn "LLVM toolchain incomplete"
    if [ "$MODE" = "install" ]; then
        # Install llvm (provides clang, clang++, lldb)
        if ! [ -x "$LLVM_PREFIX/bin/clang" ]; then
            log "Installing llvm via brew (provides clang, lldb; this may take 5-10 minutes)..."
            brew install llvm
            log "llvm: installed"
        fi
        # Install lld (separate package since LLVM 19+)
        if ! [ -x "$BREW_PREFIX/bin/lld" ] && ! [ -x "$LLVM_PREFIX/bin/lld" ]; then
            log "Installing lld via brew (separate package since LLVM 19+)..."
            brew install lld
            log "lld: installed"
        fi
        # Re-verify (only the build tools; lldb is optional, see above).
        MISSING=false
        for tool in clang clang++; do
            if [ ! -x "$LLVM_PREFIX/bin/$tool" ]; then
                err "$tool still not found at $LLVM_PREFIX/bin/$tool after install"
                MISSING=true
            fi
        done
        if ! [ -x "$BREW_PREFIX/bin/lld" ] && ! [ -x "$LLVM_PREFIX/bin/lld" ]; then
            err "lld still not found after install"
            MISSING=true
        fi
        if $MISSING; then
            err "Try: export PATH=\"$LLVM_PREFIX/bin:$BREW_PREFIX/bin:\$PATH\""
            exit 1
        fi
        log "All LLVM tools verified"
        ok
    else
        err "LLVM toolchain required. Install with: brew install llvm && brew install lld"
        exit 1
    fi
fi

# === Step 5: Python 3 ===
hdr "Step 5/5: Python 3"
if command -v python3 >/dev/null 2>&1; then
    log "Python 3: $(python3 --version)"
    ok
else
    warn "Python 3 NOT installed"
    if [ "$MODE" = "install" ] && confirm "Install Python 3 via brew?"; then
        brew install python@3.12
        log "Python 3: installed"
        ok
    else
        warn "Python 3 required by tools/build/make.py (preinstalled on macOS 14+)"
    fi
fi

# === Generate env file ===
hdr "Generating cross-build env file"
mkdir -p "$OBJ_DIR" 2>/dev/null || warn "Could not create $OBJ_DIR (will try anyway)"

# Resolve lld path (separate package since LLVM 19+)
if [ -x "$BREW_PREFIX/bin/lld" ]; then
    _XLD="$BREW_PREFIX/bin/lld"
elif [ -x "$LLVM_PREFIX/bin/lld" ]; then
    _XLD="$LLVM_PREFIX/bin/lld"
else
    _XLD="$BREW_PREFIX/bin/lld"
fi

cat > "$ENV_FILE" << EOF
# Generated by tools/cross-build/macos.sh on $(date)
# Source this file before cross-building: . $ENV_FILE

export PATH="$LLVM_PREFIX/bin:$BREW_PREFIX/bin:\$PATH"
export XCC="$LLVM_PREFIX/bin/clang"
export XCXX="$LLVM_PREFIX/bin/clang++"
export XLD="$_XLD"
export XAS="$LLVM_PREFIX/bin/clang"
export XAR="$LLVM_PREFIX/bin/llvm-ar"
export XNM="$LLVM_PREFIX/bin/llvm-nm"
export XOBJCOPY="$LLVM_PREFIX/bin/llvm-objcopy"
export XRANLIB="$LLVM_PREFIX/bin/llvm-ranlib"
export XSTRINGS="$LLVM_PREFIX/bin/llvm-strings"
export XSIZE="$LLVM_PREFIX/bin/llvm-size"

export TARGET="$TARGET"
export TARGET_ARCH="$TARGET_ARCH"
export MAKEOBJDIRPREFIX="$OBJ_DIR"

# Convenience
export OCIFBSD_ENV_FILE="$ENV_FILE"
export OCIFBSD_OBJ_DIR="$OBJ_DIR"
EOF

log "Env file written: $ENV_FILE"

# === Done ===
if [ "$MODE" = "check" ]; then
    hdr "Check complete"
    echo "All required tools present. To activate the cross-build environment:"
    echo "  . $ENV_FILE"
    exit 0
fi

hdr "Bootstrap complete"
echo ""
echo "Next steps:"
echo "  1. Source the env file:    . $ENV_FILE"
echo "  2. Cross-build userland:   python3 tools/build/make.py TARGET=$TARGET TARGET_ARCH=$TARGET_ARCH -j$JOBS libraries"
echo "  3. Build ocifbsd:          bmake -C usr.sbin/ocifbsd TARGET=$TARGET TARGET_ARCH=$TARGET_ARCH"
echo "  4. Build tests:            bmake -C tests/usr.sbin/ocifbsd TARGET=$TARGET TARGET_ARCH=$TARGET_ARCH"
echo ""
echo "Or run the all-in-one build (preferred):"
echo "  . $ENV_FILE && make -C usr.sbin/ocifbsd cross-build"
echo ""
warn "Remember: push to feature/oci-bootstrap requires explicit user approval."
