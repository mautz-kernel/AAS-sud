# AAS-sud — Sailfish OS Integration

This directory contains experimental scripts for enabling and disabling root access in Android AppSupport (AAS) on Sailfish OS.

The implementation uses a modified version of [Corellium sud](https://github.com/corellium/sud) with a UID-based allowlist.

## Tested environment

- Device: Jolla Phone 2026
- Sailfish OS: 5.2.0.20
- Android AppSupport: Android 15
- Architecture: ARM64

Other devices and Sailfish OS versions have not been tested.

## How it works

The implementation consists of three components:

**1. Modified sud**

The modified `sud` daemon denies root requests by default.

Allowed Android UIDs are read from `/data/aas-sud-allow.uids`. The file must be owned by root, have permissions `0600`, and contain one numeric UID per line.

Changes to the allowlist take effect without restarting the daemon.

**2. OverlayFS**

The `su` executable is made available at `/system/bin/su` through an OverlayFS mount.

The original Android system partition is not modified.

**3. Sailfish control scripts**

`aas-root-on`:

- Checks that the required binaries exist.
- Mounts OverlayFS if necessary.
- Starts the `sud-allowlist` daemon using Sailfish systemd.
- Verifies that the daemon is running.

`aas-root-off`:

- Stops the daemon through systemd.
- Removes the OverlayFS mount.
- Verifies that `/system/bin/su` is no longer available.
- Removes the obsolete `/dev/com.koushikdutta.superuser.daemon` directory and verifies its removal.

Both scripts have been tested without restarting Android AppSupport.

## Current file locations and permissions

The tested setup uses the following paths **inside Android AppSupport**:

| File or directory | Path | Owner | Mode |
|---|---|---|---|
| Protected directory | `/data/local/aas-sud/` | `root:root` | `0700` |
| Modified sud binary | `/data/local/aas-sud/sud-allowlist` | `root:root` | `0755` |
| OverlayFS lower directory | `/data/local/aas-sud/extra/` | `root:root` | `0755` |
| OverlayFS su executable | `/data/local/aas-sud/extra/su` | `root:root` | `0755` |
| UID allowlist | `/data/aas-sud-allow.uids` | `root:root` | `0600` |

After placing the binaries and allowlist in their locations, set and verify ownership and permissions as root **inside AAS, with the root overlay inactive**:

```sh
chown root:root /data/local/aas-sud /data/local/aas-sud/extra \
  /data/local/aas-sud/sud-allowlist /data/local/aas-sud/extra/su
chmod 700 /data/local/aas-sud
chmod 755 /data/local/aas-sud/extra \
  /data/local/aas-sud/sud-allowlist /data/local/aas-sud/extra/su
chown root:root /data/aas-sud-allow.uids
chmod 600 /data/aas-sud-allow.uids
```

**Important OverlayFS detail:** Although the parent `aas-sud/` directory must remain mode `0700`, its `extra/` directory must be mode `0755`. The mount presents that directory at `/system/bin`; if `extra/` is mode `0700`, ordinary Android UIDs can lose access even to `/system/bin/sh`. Mode `0755` permits execution and traversal without permitting non-root writes; the protected parent remains inaccessible to UID 2000.

The scripts use the transient Sailfish systemd unit `aas-sud-test.service`. These names are still prototype-specific and may change in later versions.

## Usage

After manually preparing the required files and starting Android AppSupport, run the scripts as root on the Sailfish side.

Enable root:

`sudo sh sailfish/aas-root-on`

Disable root:

`sudo sh sailfish/aas-root-off`

The scripts require `/usr/sbin/appsupport-attach`, systemd and a working OverlayFS implementation.

There is currently no automatic installer.

## Verified behavior

Root access has been tested with:

- Universal Installer
- Root Checker

The following behavior has been verified:

- Allowed UIDs can obtain root access.
- UIDs not included in the allowlist are denied.
- Allowlist changes take effect without restarting the daemon.
- Disabling root prevents new root requests.
- Root access can be enabled again without restarting AAS.

## Limitations and security notes

This is an experimental implementation, not a security-audited root solution.

- Permissions are assigned per Android UID, not per package.
- Apps sharing a UID also share the corresponding root permission.
- Disabling root does not automatically terminate processes that already have root privileges.
- Android AppSupport restarts remove the temporary OverlayFS mount and stop the running daemon. Root must be enabled again afterward.
- The scripts currently rely on manually prepared, root-owned files and prototype-specific paths.
- Reuse of an Android UID after app removal must be considered when managing allowlist entries.

The modified daemon and its surrounding setup require further security review before being recommended for general use.

## Status

The native Sailfish OS QML/Silica user interface is under development.

Currently, root access is controlled using the shell scripts described above.

## Credits and license

Based on [Corellium sud](https://github.com/corellium/sud).

The original sud project is licensed under the Apache License 2.0.

Original copyright and license notices are retained.
