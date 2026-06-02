# FreeBSD VM Provisioning Guide (for `ocifbsd` Testing)

> **Purpose:** Reproducible FreeBSD VM provisioning for testing the `ocifbsd` runtime from a macOS host. Covers UTM, QEMU, and VirtualBox. Includes snapshot-based clean-state management.
>
> **Last updated:** 2026-06-02 (oci-bootstrap work plan, T26)

---

## Overview

Testing `ocifbsd` requires a **real FreeBSD kernel** because the runtime depends on:

- `jail(8)` — process and filesystem isolation
- VNET — per-jail network stack
- ZFS — snapshot-based image layer management
- `capsicum(4)` — capability mode for security
- `pf(4)` — packet filter for NAT/networking

None of these are available on macOS. You must test in a FreeBSD VM.

This guide covers three VM technologies, all supported by FreeBSD 14.x and 15.x. Choose based on your platform and workflow:

| VM | Best for | Snapshot command |
|----|----------|------------------|
| **UTM** | Apple Silicon, daily use | GUI: File → Save Snapshot |
| **QEMU** | Scripted CI, cross-platform | `qemu-img snapshot -c/-a` |
| **VirtualBox** | Intel macOS, stable scripting | `VBoxManage snapshot take/restore` |

---

## 1. Base Image

Download a FreeBSD 14.x or 15.x ISO from <https://www.freebsd.org/releases/>. Recommended:

- **FreeBSD 14.2-RELEASE-amd64-dvd1.iso** (stable, ZFS-root, well-tested)
- **FreeBSD 15.0-RELEASE-amd64-disc1.iso** (newer, more features)

You do NOT need a pre-built VM image — all three technologies support installing from the ISO in ~10 minutes.

### 1.1. Required VM resources

| Resource | Minimum | Recommended |
|----------|---------|-------------|
| vCPUs | 2 | 4 |
| RAM | 2 GB | 4 GB |
| Disk | 20 GB | 40 GB |
| Network | NAT or Bridged | Bridged (for cluster testing) |
| Display | 1280×720 | 1920×1080 |

ZFS requires at least 2 GB RAM and a 2 GB disk for the boot pool. Use `ZFS` partitioning during install.

---

## 2. UTM (Apple Silicon recommended)

UTM is the fastest VM host on Apple Silicon, using Apple's Hypervisor.framework via QEMU under the hood.

### 2.1. Install UTM

```bash
brew install --cask utm
# Or download from https://mac.getutm.app/
```

### 2.2. Create the VM

1. Open UTM → Click **+** → **Virtualize** → **FreeBSD**
2. **System:** Architecture = `x86_64` (for FreeBSD amd64 builds), Memory = 4096 MB
3. **Drives:** New → Removable = No, Size = 40 GB
4. **Network:** Shared Network (NAT) for internet, or Bridged for cluster testing
5. **Display:** 1920×1080
6. Save as `freebsd-oci`

### 2.3. Install FreeBSD

1. Download `FreeBSD-14.2-RELEASE-amd64-dvd1.iso` and mount it as a CD-ROM in the VM
2. Boot the VM
3. **Installer:**
   - Keymap: your locale
   - Hostname: `freebsd-oci`
   - Partitioning: **Auto (ZFS)** → `stripe` → disk0
   - Network: **DHCP** (default)
   - Root password: set it (you'll need it for SSH)
   - Time zone: UTC
   - Services: enable `sshd`, `ntpd`
   - Add user: `oci` with `wheel` group
4. After install, **reboot** and remove the ISO

### 2.4. Post-install setup

```sh
# SSH in (use the VM's IP from `ifconfig` or your DHCP server)
ssh root@freebsd-oci.local

# Update the base system
freebsd-update fetch install
pkg update
pkg upgrade -y

# Install build dependencies
pkg install -y git bmake llvm18 bash

# Enable jail features
sysrc jail_enable=YES
sysrc jail_list=""
service jail start

# Enable pf for NAT/networking
sysrc pf_enable=YES
service pf start

# Optional: install ZFS-friendly tools
pkg install -y sysutils/zfs-snapshot-clean
```

### 2.5. Take a clean snapshot

1. Shut down the VM (Actions → Stop, or `poweroff` in the VM)
2. **File → Save Snapshot** → name it `clean`
3. Description: "Fresh FreeBSD 14.2 with OCI build deps installed"

### 2.6. Restore to clean state

1. Shut down the VM
2. **File → Restore Snapshot** → select `clean`
3. Boot the VM

### 2.7. SSH access (recommended)

For `scp` and automated test loops, use SSH:

```sh
# In the VM, enable password-less root login (for test VMs only)
echo "PermitRootLogin yes" >> /etc/ssh/sshd_config
service sshd restart

# From macOS
ssh-copy-id root@freebsd-oci.local
scp usr.sbin/ocifbsd/ocifbsd root@freebsd-oci:/root/
```

---

## 3. QEMU (scripted CI)

QEMU is the best choice for scripted snapshot management and CI integration.

### 3.1. Install QEMU

```bash
brew install qemu
# Verify
qemu-system-x86_64 --version
qemu-img --version
```

### 3.2. Create the disk image

```bash
qemu-img create -f qcow2 freebsd-oci.qcow2 40G
```

### 3.3. Install FreeBSD

```bash
# Start a one-time install VM
qemu-system-x86_64 \
  -m 4096 \
  -smp 2 \
  -hda freebsd-oci.qcow2 \
  -cdrom /path/to/FreeBSD-14.2-RELEASE-amd64-dvd1.iso \
  -boot d \
  -netdev user,id=net0 \
  -device e1000,netdev=net0 \
  -vnc :0

# Connect with VNC: open vnc://localhost:5900
# Run the FreeBSD installer as in § 2.3
# After install, shutdown the VM
```

### 3.4. Take a clean snapshot

```bash
qemu-img snapshot -c clean freebsd-oci.qcow2
# Verify
qemu-img snapshot -l freebsd-oci.qcow2
# Expected: snapshot ID 1, tag "clean", size info
```

### 3.5. Boot the VM

```bash
qemu-system-x86_64 \
  -m 4096 \
  -smp 2 \
  -hda freebsd-oci.qcow2 \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device e1000,netdev=net0 \
  -vnc :0

# SSH via forwarded port:
ssh -p 2222 root@localhost
```

### 3.6. Restore to clean state

```bash
# Shut down the VM first, then:
qemu-img snapshot -a clean freebsd-oci.qcow2
# Verify the snapshot was applied:
qemu-img snapshot -l freebsd-oci.qcow2
```

### 3.7. Delete the snapshot

```bash
qemu-img snapshot -d clean freebsd-oci.qcow2
```

---

## 4. VirtualBox (Intel macOS)

VirtualBox is the most stable scripted option on Intel macOS.

### 4.1. Install VirtualBox

```bash
brew install --cask virtualbox
```

### 4.2. Create the VM

```bash
VBoxManage createvm --name freebsd-oci --ostype FreeBSD_64 --register
VBoxManage modifyvm freebsd-oci \
  --memory 4096 \
  --cpus 2 \
  --nic1 nat \
  --boot1 dvd \
  --boot2 disk
VBoxManage createhd --filename freebsd-oci.vdi --size 40960
VBoxManage storagectl freebsd-oci --name SATA --add sata --controller IntelAhci
VBoxManage storageattach freebsd-oci --storagectl SATA \
  --port 0 --type hdd --medium freebsd-oci.vdi
VBoxManage storageattach freebsd-oci --storagectl SATA \
  --port 1 --type dvddrive --medium /path/to/FreeBSD-14.2-RELEASE-amd64-dvd1.iso

# Start the VM
VBoxManage startvm freebsd-oci
# Or use the VirtualBox GUI
```

### 4.3. Install FreeBSD

Same as § 2.3. After install, remove the ISO:

```bash
VBoxManage storageattach freebsd-oci --storagectl SATA \
  --port 1 --type dvddrive --medium none
```

### 4.4. Take a clean snapshot

```bash
VBoxManage snapshot freebsd-oci take clean \
  --description "Fresh FreeBSD 14.2 with OCI build deps"
```

### 4.5. Restore to clean state

```bash
# Shut down the VM first, then:
VBoxManage snapshot freebsd-oci restore clean
```

### 4.6. SSH port forwarding

```bash
# After VM is running, forward host:2222 → guest:22
VBoxManage modifyvm freebsd-oci --natpf1 "ssh,tcp,,2222,,22"
# SSH from macOS:
ssh -p 2222 root@localhost
```

---

## 5. Test Loop Pattern

The snapshot-based test loop is:

```bash
# 1. Build on macOS
bmake -C usr.sbin/ocifbsd TARGET=amd64 TARGET_ARCH=amd64

# 2. Copy to VM
scp usr.sbin/ocifbsd/ocifbsd freebsd-oci:/root/

# 3. Run test in VM
ssh freebsd-oci /root/ocifbsd_test

# 4. Restore clean snapshot (for the next test)
#    UTM: GUI
#    QEMU: qemu-img snapshot -a clean freebsd-oci.qcow2
#    VirtualBox: VBoxManage snapshot freebsd-oci restore clean

# 5. Repeat
```

For an automated wrapper, see `.omo/drafts/vm-smoke-test.sh` (T26 of the oci-bootstrap work plan).

---

## 6. Snapshot Best Practices

1. **Always take the snapshot AFTER `pkg update` and `freebsd-update install`.** A snapshot taken before updates will get stale fast.
2. **Keep the snapshot small.** Don't include `/tmp` test artifacts in the clean state.
3. **Tag the snapshot with a description.** Future-you will thank present-you.
4. **Test the restore.** After taking a snapshot, restore it once to verify the restore works.
5. **Use multiple snapshots for different test stages.** E.g., `clean`, `ocifbsd-installed`, `cluster-up`.

---

## 7. Troubleshooting

### 7.1. VM cannot reach the internet

**Symptom:** `pkg install` fails with "No address record".

**Fix:** Check the network adapter. For NAT (recommended for test VMs), ensure the VM's `/etc/resolv.conf` has a valid nameserver:

```sh
cat /etc/resolv.conf
# Should have: nameserver 8.8.8.8 (or your router's IP)
```

### 7.2. ZFS pool not mounting

**Symptom:** After restore, `zpool import` shows the pool but it's not mounted.

**Fix:**
```sh
zpool import -f zroot
zfs mount -a
```

### 7.3. VM boots to single-user mode

**Symptom:** VM shows `mountroot>` prompt.

**Cause:** The disk device changed (e.g., from `ada0` to `ada1` after a re-attach).

**Fix:** At the `mountroot>` prompt, type `?` to list devices, then `ufs:/dev/ada0p2` or `zfs:zroot/ROOT/default` (whichever matches).

### 7.4. SSH connection refused after restore

**Symptom:** `ssh: connect to host freebsd-oci port 22: Connection refused`.

**Cause:** `sshd` is not enabled in `/etc/rc.conf`.

**Fix:**
```sh
ssh root@freebsd-oci 'sysrc sshd_enable=YES && service sshd start'
```

---

## See Also

- [`.plan/020.0-Developer-Setup.md`](../../.plan/020.0-Developer-Setup.md) — Full developer setup
- [`.omo/drafts/vm-smoke-test.sh`](./vm-smoke-test.sh) — Automated test loop script
- [FreeBSD Handbook — Virtualization](https://docs.freebsd.org/en/books/handbook/virtualization/)
- [UTM Documentation](https://docs.getutm.app/)
- [QEMU Documentation](https://www.qemu.org/docs/master/)
- [VirtualBox Manual](https://www.virtualbox.org/manual/)
