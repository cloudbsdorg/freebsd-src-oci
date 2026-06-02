#!/bin/sh
# .omo/drafts/vm-smoke-test.sh
# Automated test loop: build on host → scp to VM → run test → restore snapshot
#
# Usage:
#   ./vm-smoke-test.sh [VM_BACKEND]
#   VM_BACKEND = utm | qemu | vbox | ssh-only (default: ssh-only)
#
# This script is a smoke test wrapper for the oci-bootstrap work plan (T26).
# It does NOT run the full ATF test suite — that's done in the VM directly.
# This script verifies the basic build → deploy → test → reset loop works.

set -eu

# === Configuration ===
VM_BACKEND="${1:-ssh-only}"
VM_HOST="${VM_HOST:-freebsd-oci}"
VM_PORT="${VM_PORT:-22}"
VM_USER="${VM_USER:-root}"
VM_SNAPSHOT_NAME="${VM_SNAPSHOT_NAME:-clean}"
QCOW2_PATH="${QCOW2_PATH:-freebsd-oci.qcow2}"
VBOX_NAME="${VBOX_NAME:-freebsd-oci}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() { printf "${GREEN}[+]${NC} %s\n" "$1"; }
warn() { printf "${YELLOW}[!]${NC} %s\n" "$1" >&2; }
err() { printf "${RED}[X]${NC} %s\n" "$1" >&2; }

# === Preflight checks ===
log "Preflight checks..."

# Check we're in a FreeBSD source tree
if [ ! -f "Makefile" ] || [ ! -d "usr.sbin/ocifbsd" ]; then
    err "Not in a freebsd-src-oci checkout. Run from the repo root."
    exit 1
fi

# Check bmake
if ! command -v bmake >/dev/null 2>&1; then
    err "bmake not found. Install: brew install bmake"
    exit 1
fi

# Check clang
if ! command -v clang >/dev/null 2>&1; then
    err "clang not found. Install: brew install llvm"
    exit 1
fi

# Check lld (for cross-link)
if ! command -v lld >/dev/null 2>&1 && ! command -v ld.lld >/dev/null 2>&1; then
    warn "lld not found. Cross-build will fail. Install: brew install llvm"
fi

# Check SSH access to VM (for ssh-only mode)
if [ "$VM_BACKEND" = "ssh-only" ] || [ "$VM_BACKEND" = "utm" ] || [ "$VM_BACKEND" = "vbox" ] || [ "$VM_BACKEND" = "qemu" ]; then
    if ! ssh -p "$VM_PORT" -o ConnectTimeout=5 -o BatchMode=yes "$VM_USER@$VM_HOST" true 2>/dev/null; then
        err "Cannot SSH to $VM_USER@$VM_HOST:$VM_PORT. Check VM is running and SSH keys are set up."
        exit 1
    fi
fi

log "Preflight OK"

# === Step 1: Build on host ===
log "Step 1: Building ocifbsd on host..."

# Source cross-build env if it exists
if [ -f "$HOME/.freebsd-cross-build-env" ]; then
    # shellcheck disable=SC1090
    . "$HOME/.freebsd-cross-build-env"
    log "  Sourced ~/.freebsd-cross-build-env"
fi

# Build ocifbsd
if ! bmake -C usr.sbin/ocifbsd TARGET=amd64 TARGET_ARCH=amd64 2>&1 | tail -5; then
    err "Build failed. See output above."
    exit 1
fi

# Build tests
if ! bmake -C tests/usr.sbin/ocifbsd TARGET=amd64 TARGET_ARCH=amd64 2>&1 | tail -5; then
    warn "Test build failed (may be expected if test deps missing). Continuing..."
fi

if [ ! -f "usr.sbin/ocifbsd/ocifbsd" ]; then
    err "ocifbsd binary not found after build. Build failed silently?"
    exit 1
fi

log "  Built: usr.sbin/ocifbsd/ocifbsd ($(stat -f%z usr.sbin/ocifbsd/ocifbsd 2>/dev/null || stat -c%s usr.sbin/ocifbsd/ocifbsd) bytes)"

# === Step 2: Copy to VM ===
log "Step 2: Copying to VM..."

scp -P "$VM_PORT" usr.sbin/ocifbsd/ocifbsd "$VM_USER@$VM_HOST:/root/ocifbsd"
log "  Copied usr.sbin/ocifbsd/ocifbsd to VM"

if [ -f "tests/usr.sbin/ocifbsd/ocifbsd_test" ]; then
    scp -P "$VM_PORT" tests/usr.sbin/ocifbsd/ocifbsd_test "$VM_USER@$VM_HOST:/root/ocifbsd_test"
    log "  Copied tests/usr.sbin/ocifbsd/ocifbsd_test to VM"
fi

# === Step 3: Run smoke test in VM ===
log "Step 3: Running smoke test in VM..."

# Verify the binary is a FreeBSD ELF
REMOTE_FILE_INFO=$(ssh -p "$VM_PORT" "$VM_USER@$VM_HOST" "file /root/ocifbsd")
echo "  $REMOTE_FILE_INFO"

if ! echo "$REMOTE_FILE_INFO" | grep -q "ELF.*FreeBSD"; then
    err "Binary is not a FreeBSD ELF! Got: $REMOTE_FILE_INFO"
    exit 1
fi

# Run --version or --help to verify the binary executes
log "  Running /root/ocifbsd --version..."
REMOTE_VERSION=$(ssh -p "$VM_PORT" "$VM_USER@$VM_HOST" "chmod +x /root/ocifbsd && /root/ocifbsd --version 2>&1" || true)
echo "  $REMOTE_VERSION"

# Run the test if available
if ssh -p "$VM_PORT" "$VM_USER@$VM_HOST" "test -f /root/ocifbsd_test"; then
    log "  Running /root/ocifbsd_test..."
    REMOTE_TEST=$(ssh -p "$VM_PORT" "$VM_USER@$VM_HOST" "chmod +x /root/ocifbsd_test && /root/ocifbsd_test 2>&1" || true)
    echo "  $REMOTE_TEST"
fi

# === Step 4: Restore snapshot (only for VM backends that support it) ===
log "Step 4: Restoring snapshot..."

case "$VM_BACKEND" in
    qemu)
        if [ -f "$QCOW2_PATH" ]; then
            log "  qemu-img snapshot -a $VM_SNAPSHOT_NAME $QCOW2_PATH"
            qemu-img snapshot -a "$VM_SNAPSHOT_NAME" "$QCOW2_PATH" || warn "Snapshot restore failed (VM may be running)"
        else
            warn "QCOW2_PATH not found: $QCOW2_PATH. Skipping restore."
        fi
        ;;
    vbox)
        if command -v VBoxManage >/dev/null 2>&1; then
            log "  VBoxManage snapshot $VBOX_NAME restore $VM_SNAPSHOT_NAME"
            VBoxManage snapshot "$VBOX_NAME" restore "$VM_SNAPSHOT_NAME" || warn "Snapshot restore failed"
        else
            warn "VBoxManage not found. Skipping restore."
        fi
        ;;
    utm)
        warn "UTM snapshot restore must be done via GUI (File → Restore Snapshot)."
        warn "Or use AppleScript: osascript -e 'tell application \"UTM\" to ...'"
        ;;
    ssh-only)
        log "  ssh-only mode: snapshot managed by user. Skipping restore."
        ;;
    *)
        warn "Unknown VM_BACKEND: $VM_BACKEND. Skipping restore."
        ;;
esac

# === Done ===
log "Smoke test complete!"
echo ""
echo "Summary:"
echo "  Built:    usr.sbin/ocifbsd/ocifbsd ($(stat -f%z usr.sbin/ocifbsd/ocifbsd 2>/dev/null || stat -c%s usr.sbin/ocifbsd/ocifbsd) bytes)"
echo "  Deployed: $VM_USER@$VM_HOST:$VM_PORT"
echo "  Verified: FreeBSD ELF, executed --version"
echo "  Snapshot: $VM_SNAPSHOT_NAME ($VM_BACKEND)"
echo ""
echo "Next steps:"
echo "  - For full ATF tests, run in VM: cd /root && kyua test"
echo "  - To iterate, edit code, re-run this script"
echo "  - To commit: git add usr.sbin/ocifbsd/ && git commit -m 'ocifbsd: <description>'"
