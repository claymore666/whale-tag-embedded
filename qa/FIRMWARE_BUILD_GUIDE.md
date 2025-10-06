# Firmware Build Guide

Complete guide for building whale tag firmware SD card images.

## Overview

The build process creates a complete SD card image (`out/sdcard.img`) containing:
- Raspbian OS (Bookworm/Debian 12)
- Custom Debian packages (ceti-tag-data-capture, ceti-tag-set-hostname)
- System configuration and overlays
- Three partitions: boot (FAT32), rootfs (ext4), data (ext4)

## Prerequisites

### System Requirements

- Linux host (tested on Debian/Ubuntu)
- sudo access (required for loop device manipulation)
- Interactive terminal (TTY required)
- Docker installed and running
- At least 10GB free disk space

### Install Dependencies

```bash
sudo apt-get install dos2unix binfmt-support qemu-system-common qemu-user-static docker.io
```

## Build Process

### 1. Clean Build (Recommended)

Start with a clean state:

```bash
# Remove old build artifacts
make clean

# Optional: Deep clean (removes Docker images too)
make deep_clean
```

### 2. Run Build

**IMPORTANT:** Must be run in an interactive terminal with sudo:

```bash
sudo make build
```

**Why sudo is required:**
- Creates loop devices for partition manipulation
- Mounts filesystems
- Modifies file permissions

**Why interactive terminal is required:**
- Docker needs TTY for interactive user setup
- Build process may prompt for confirmations

### 3. Build Steps (Automated)

The build system performs these steps automatically:

1. **Build Docker image** (`sdcard-builder`)
   - Based on `debian:bookworm`
   - Includes e2fsck 1.47+ for modern filesystem support
   - Installs ARM64 emulation (qemu-user-static)

2. **Download base OS image**
   - Fetches latest `raspios-bookworm-arm64-lite`
   - Cached in `raspios/raspios.img`
   - Downloads only once, reused for subsequent builds

3. **Create environment image** (`raspios/environment.img`)
   - Copies base image
   - Expands rootfs partition by 512MB
   - Appends 128MB data partition (ext4, label: cetiData)
   - Runs `build/setup_image.sh` to configure system:
     - Installs required packages
     - Sets up journald logging
     - Configures CPU isolation
     - Sets hostname script
     - Creates systemd services

4. **Build Debian packages**
   - Builds in QEMU ARM64 environment
   - Packages:
     - `ceti-tag-data-capture` - Main firmware
     - `ceti-tag-set-hostname` - MAC-based hostname setter
   - Output: `out/*.deb`

5. **Create final image** (`out/sdcard.img`)
   - Copies environment image
   - Installs Debian packages
   - Finalizes configuration

### 4. Build Output

```
out/
├── sdcard.img                              # Final SD card image (5-6GB)
├── ceti-tag-data-capture_X.X-X_all.deb    # Firmware package
└── ceti-tag-set-hostname_X.X-X_all.deb    # Hostname package

raspios/
├── raspios.img         # Base OS (cached, reused)
└── environment.img     # Intermediate build artifact
```

## Common Issues and Solutions

### Issue: Loop device errors

**Symptom:**
```
subprocess.CalledProcessError: Command '['losetup', '--find', '--show', ...
```

**Cause:** Stale loop devices from previous builds/tests

**Solution:**
```bash
# Check current loop devices
sudo losetup -l

# Clean up all loop devices
sudo losetup -D

# Remove stopped Docker containers
docker container prune -f

# Retry build
sudo make build
```

### Issue: e2fsck version error

**Symptom:**
```
e2fsck: Get a newer version of e2fsck!
/dev/loop32 has unsupported feature(s): FEATURE_C12
```

**Cause:** Docker image using old Debian (Bullseye) with e2fsck 1.46

**Solution:** Ensure you've merged the Bookworm migration:
```bash
git merge fix/migrate-bullseye-to-bookworm --no-edit
```

This updates:
- `build/Dockerfile`: `debian:bullseye` → `debian:bookworm`
- `Makefile`: `raspios-bullseye-arm64-lite` → `raspios-bookworm-arm64-lite`

### Issue: "not a TTY" error

**Symptom:**
```
the input device is not a TTY
```

**Cause:** Running build through automation/script without TTY

**Solution:** Run `sudo make build` directly in your terminal, not through automated scripts

### Issue: Docker permission denied

**Symptom:**
```
permission denied while trying to connect to Docker daemon
```

**Solution:**
```bash
# Add user to docker group
sudo usermod -aG docker $USER

# Log out and back in, or:
newgrp docker
```

### Issue: Out of disk space

**Symptom:**
```
No space left on device
```

**Solution:**
```bash
# Check disk usage
df -h

# Clean old Docker images
docker image prune -a

# Clean build artifacts
make deep_clean
```

## Partial Builds

### Build only Debian packages

```bash
make packages
```

Skips image creation, only builds `.deb` files.

### Build specific package

```bash
cd packages/ceti-tag-data-capture
make build          # Build firmware binaries
make test           # Run unit tests
make clean          # Clean build artifacts
```

**Note:** Package builds require ARM64 cross-compilation or QEMU emulation.

## Updating Firmware After Code Changes

After merging new firmware code (e.g., depth-aware burnwire feature):

1. **Ensure code is merged into current branch:**
   ```bash
   git merge feature/depth-aware-burnwire --no-edit
   ```

2. **Clean previous build:**
   ```bash
   make clean
   ```

3. **Rebuild image:**
   ```bash
   sudo make build
   ```

4. **Expand for testing** (one-time after build):
   ```bash
   sudo ./qa/expand_image.sh
   ```

## Testing the Built Image

### 1. Expand image for testing

The default image is sized exactly for content. Expand it to allow log files:

```bash
sudo ./qa/expand_image.sh
```

This adds 1GB to the image.

### 2. Run end-to-end tests

```bash
cd qa/libpigpio_sim
make arm64                           # Build LD_PRELOAD library

cd ../..
./test_prs_complete_e2e.sh          # Complete E2E test suite
```

Or test specific features:

```bash
./qa/test_scripts/test_depth_aware_burnwire.sh
```

## Build System Architecture

```
Makefile (top-level)
│
├─> build/Dockerfile                # Docker image definition
├─> build/rpi-image                 # Python tool for image manipulation
├─> build/setup_image.sh            # System configuration script
├─> build/make_dpkg.sh              # Debian package builder
├─> build/install_dpkg.sh           # Package installer
│
├─> packages/ceti-tag-data-capture/ # Main firmware package
│   ├─> debian/                     # Debian package metadata
│   ├─> src/                        # C source code
│   └─> Makefile                    # Package build system
│
├─> packages/ceti-tag-set-hostname/ # Hostname setter package
│   └─> debian/
│
└─> overlay/                        # Filesystem overlays
    ├─> boot/                       # Boot partition files
    └─> etc/                        # System configuration
```

## Build Time

Typical build times:
- **First build:** 45-90 minutes (downloads base image, builds everything)
- **Incremental build:** 15-30 minutes (reuses cached base image)
- **Package-only build:** 5-10 minutes

## Troubleshooting Tips

1. **Check Docker logs:**
   ```bash
   docker logs <container_id>
   ```

2. **Enter build environment manually:**
   ```bash
   make docker-shell
   ```

3. **Verify base image:**
   ```bash
   ls -lh raspios/raspios.img
   # Should be ~2.5GB for Bookworm
   ```

4. **Check available loop devices:**
   ```bash
   sudo losetup -l
   # Should see available /dev/loopX devices
   ```

5. **Monitor disk space during build:**
   ```bash
   watch -n 5 'df -h | grep -E "Filesystem|/dev/sd"'
   ```

## Migration Notes (Bullseye → Bookworm)

The build system was migrated from Debian 11 (Bullseye) to Debian 12 (Bookworm) because:

1. Raspberry Pi Foundation no longer provides Bullseye images
2. Newer e2fsck required for modern filesystem features
3. Bookworm uses journald instead of rsyslog (persistent logging to `/data/journal`)

**Key changes:**
- Base OS: `raspios-bullseye-arm64-lite` → `raspios-bookworm-arm64-lite`
- Docker base: `debian:bullseye` → `debian:bookworm`
- Package renames: `qemu` → `qemu-user-static`, `netcat` → `netcat-openbsd`
- Logging: rsyslog → systemd-journald

All hardware drivers remain compatible (same kernel 6.1.x lineage).

## References

- **CLAUDE.md** - Project overview and build system introduction
- **qa/QA_TESTING_GUIDE.md** - Testing firmware with LD_PRELOAD simulation
- **Issue #113** - Bookworm migration tracking
- Build tools source: `build/rpi-image` (Python image manipulation tool)
