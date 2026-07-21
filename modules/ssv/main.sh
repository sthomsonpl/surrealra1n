#!/bin/bash

SKIP_SETUP_DEV=${SKIP_SETUP_DEV:-0}
SSHD_DEV=${SSHD_DEV:-0}

ssv_reset_options() {
    SKIP_SETUP_DEV=0
    SSHD_DEV=0
}

ssv_deployment_target() {
    local major=${VERSION%%.*}
    if [[ $major =~ ^[0-9]+$ ]]; then
        printf '%s.0\n' "$major"
    else
        printf '15.0\n'
    fi
}

ssv_print_menu_options() {
    if [[ $SKIP_SETUP_DEV -eq 1 ]]; then
        echo "4. Experimental SSV Skip Setup (A13 tested; activation required) [ON]"
    else
        echo "4. Experimental SSV Skip Setup (A13 tested; activation required) [OFF]"
    fi
    if [[ $SSHD_DEV -eq 1 ]]; then
        echo "5. Experimental SSV SSH patches (A13 tested) [ON]"
    else
        echo "5. Experimental SSV SSH patches (A13 tested) [OFF]"
    fi
}

ssv_select_menu_option() {
    case "$1" in
        4) ssv_toggle_skip_setup ;;
        5) ssv_toggle_ssh ;;
        *) return 1 ;;
    esac
}

ssv_set_custom_ipsw_name() {
    if [[ $SKIP_SETUP_DEV -eq 1 && $SSHD_DEV -eq 1 ]]; then
        CUSTOM_IPSW_NAME="custom_ssv_skip_setup_ssh.ipsw"
    elif [[ $SKIP_SETUP_DEV -eq 1 ]]; then
        CUSTOM_IPSW_NAME="custom_ssv_skip_setup.ipsw"
    elif [[ $SSHD_DEV -eq 1 ]]; then
        CUSTOM_IPSW_NAME="custom_ssv_ssh.ipsw"
    else
        CUSTOM_IPSW_NAME="custom.ipsw"
    fi
}

write_ssv_patch_profile() {
    local profile_directory="boot/profiles/$ECID"
    local profile_path="$profile_directory/$VERSION.json"
    mkdir -p "$profile_directory"
    python3 - "$profile_path" "$ECID" "$IDENTIFIER" "$VERSION" \
        "$SKIP_SETUP_DEV" "$SSHD_DEV" <<'PY'
import json
import os
import sys

path, ecid, identifier, version, skip_setup, sshd = sys.argv[1:]
skip_setup_enabled = skip_setup == "1"
loader_enabled = sshd == "1"
profile = {
    "schema_version": 1,
    "ecid": ecid,
    "identifier": identifier,
    "ios_version": version,
    "ssv_patches": {
        "enabled": skip_setup_enabled or loader_enabled,
        "skip_setup": skip_setup_enabled,
        "dropbear_sshd": loader_enabled,
        "experimental": True,
    },
    "loader": {
        "binary": "/usr/libexec/surreal_loader",
        "services_path": "/var/jb/Library/SurrealLoader/Services",
        "launch_method": "launchd_cache",
    },
}
temporary = f"{path}.surrealra1n"
with open(temporary, "w", encoding="utf-8") as output:
    json.dump(profile, output, indent=2, sort_keys=True)
    output.write("\n")
    output.flush()
    os.fsync(output.fileno())
os.replace(temporary, path)
PY
    echo "[*] Saved experimental SSV patch profile: $profile_path"
}

ssv_patch_root_hash_from_restore_log()(
    # Sync the device seal with the IPSW metadata.
    set -euo pipefail

    local ipsw_path="$1"
    local restore_log="$2"
    local hash_line=""
    local actual_hash=""
    local expected_hash=""
    local cache_loader_inode=""
    local cache_loader_original_inode=""
    local loader_inode=""
    local launchd_cache_inode=""
    local launchd_cache_original_inode=""
    local anchor_inodes=""
    local temp_dir=""
    local target_member=""
    local mtree_member=""
    local patch_root_hash=0
    local member=""
    local current_hash=""
    local absolute_ipsw=""
    local payload_size=""

    cleanup_sshd_root_hash() {
        if [[ -n "$temp_dir" && -d "$temp_dir" ]]; then
            rm -rf "$temp_dir"
        fi
    }
    trap cleanup_sshd_root_hash EXIT

    if [[ ! -f "$ipsw_path" || ! -f "$restore_log" ]]; then
        return 0
    fi

    hash_line=$(python3 - "$restore_log" <<'PY'
import re
import sys

with open(sys.argv[1], "rb") as restore_log:
    contents = restore_log.read().decode("utf-8", "replace")

# Remove the asynchronous futurerestore error before parsing.
contents = contents.replace("ERROR: Unable to successfully restore device", "")
pattern = re.compile(
    r"Invalid root hash:\s*([0-9a-fA-F\s]{64,128})"
    r"\s*\(expected:\s*([0-9a-fA-F]{64})\)",
    re.MULTILINE,
)
matches = []
for actual_field, expected in pattern.findall(contents):
    actual = re.sub(r"\s+", "", actual_field)
    if len(actual) == 64:
        matches.append((actual.lower(), expected.lower()))
inode_matches = re.findall(
    r"SURREALRAIN_MTREE_ANCHOR_INODES=(\d+):(\d+):(\d+):(\d+):(\d+)",
    contents,
)
if matches and inode_matches:
    print(*matches[-1], *inode_matches[-1])
elif matches:
    print("MISSING_INODE")
PY
    )
    if [[ -z "$hash_line" ]]; then
        return 0
    fi
    if [[ "$hash_line" == "MISSING_INODE" ]]; then
        echo "[!] APFS reported a new root hash, but the SurrealLoader inode report is missing."
        echo "[!] Refusing to patch only half of the authenticated metadata pair."
        exit 1
    fi

    read -r actual_hash expected_hash cache_loader_inode \
        cache_loader_original_inode loader_inode launchd_cache_inode \
        launchd_cache_original_inode <<< "$hash_line"

    if [[ ! "$actual_hash" =~ ^[0-9a-f]{64}$ || \
          ! "$expected_hash" =~ ^[0-9a-f]{64}$ || \
          ! "$cache_loader_inode" =~ ^[1-9][0-9]*$ || \
          ! "$cache_loader_original_inode" =~ ^[1-9][0-9]*$ || \
          ! "$loader_inode" =~ ^[1-9][0-9]*$ || \
          ! "$launchd_cache_inode" =~ ^[1-9][0-9]*$ || \
          ! "$launchd_cache_original_inode" =~ ^[1-9][0-9]*$ ]]; then
        echo "[!] Could not parse the root hash/SurrealLoader inode set from $restore_log."
        exit 1
    fi
    printf -v anchor_inodes '%s:%s:%s:%s:%s' \
        "$cache_loader_inode" "$cache_loader_original_inode" "$loader_inode" \
        "$launchd_cache_inode" "$launchd_cache_original_inode"

    temp_dir=$(mktemp -d "${TMPDIR:-/tmp}/surrealra1n-root-hash.XXXXXX")
    absolute_ipsw="$(cd "$(dirname "$ipsw_path")" && pwd)/$(basename "$ipsw_path")"

    while IFS= read -r member; do
        [[ -z "$member" ]] && continue
        unzip -p "$absolute_ipsw" "$member" > "$temp_dir/component.im4p"
        ./bin/img4 -i "$temp_dir/component.im4p" -o "$temp_dir/payload.bin"
        current_hash=$(dd if="$temp_dir/payload.bin" bs=1 skip=16 count=32 \
            2>/dev/null | xxd -p -c 64 | tr '[:upper:]' '[:lower:]')

        if [[ "$current_hash" == "$actual_hash" ]]; then
            target_member="$member"
            patch_root_hash=0
            break
        elif [[ "$current_hash" == "$expected_hash" ]]; then
            target_member="$member"
            patch_root_hash=1
            break
        fi
    done < <(unzip -Z1 "$absolute_ipsw" | grep '^Firmware/.*\.root_hash$' || true)

    if [[ -z "$target_member" ]]; then
        echo "[!] No root_hash component matched expected hash $expected_hash."
        exit 1
    fi

    echo "[*] Device reported a new APFS seal hash: $actual_hash"
    echo "[*] Device reported the SurrealLoader anchor inodes: $anchor_inodes"
    if [[ $patch_root_hash -eq 1 ]]; then
        echo "[*] Updating $target_member in place..."
        unzip -p "$absolute_ipsw" "$target_member" > "$temp_dir/component.im4p"
        ./bin/img4 -i "$temp_dir/component.im4p" -o "$temp_dir/payload.bin"
        printf '%s' "$actual_hash" | xxd -r -p > "$temp_dir/new-hash.bin"
        dd if="$temp_dir/new-hash.bin" of="$temp_dir/payload.bin" \
            bs=1 seek=16 conv=notrunc 2>/dev/null

        cp "$temp_dir/component.im4p" "$temp_dir/patched.im4p"
        ./bin/img4 -i "$temp_dir/patched.im4p" -R "$temp_dir/payload.bin"
        python3 modules/ssv/patch_stored_zip_member.py \
            "$absolute_ipsw" "$target_member" "$temp_dir/patched.im4p"
    else
        echo "[*] Developer root_hash already contains $actual_hash."
    fi

    unzip -p "$absolute_ipsw" "$target_member" > "$temp_dir/verify.im4p"
    ./bin/img4 -i "$temp_dir/verify.im4p" -o "$temp_dir/verify.bin"
    current_hash=$(dd if="$temp_dir/verify.bin" bs=1 skip=16 count=32 \
        2>/dev/null | xxd -p -c 64 | tr '[:upper:]' '[:lower:]')
    if [[ "$current_hash" != "$actual_hash" ]]; then
        echo "[!] root_hash verification failed after updating the IPSW."
        exit 1
    fi

    mtree_member="${target_member%.root_hash}.mtree"
    if ! unzip -Z1 "$absolute_ipsw" | grep -Fx "$mtree_member" >/dev/null; then
        echo "[!] Matching canonical mtree component is missing: $mtree_member"
        exit 1
    fi

    echo "[*] Rebuilding $mtree_member for the SurrealLoader anchor..."
    unzip -p "$absolute_ipsw" "$mtree_member" > "$temp_dir/mtree.im4p"
    ./bin/img4 -i "$temp_dir/mtree.im4p" -o "$temp_dir/mtree.archive"
    payload_size=$(stat -f '%z' "$temp_dir/mtree.archive")
    mkdir "$temp_dir/canonical"
    /usr/bin/aa extract \
        -i "$temp_dir/mtree.archive" \
        -d "$temp_dir/canonical"
    python3 modules/ssv/patch_canonical_mtree.py \
        "$temp_dir/canonical/mtree.txt" \
        "$cache_loader_inode" "$cache_loader_original_inode" "$loader_inode" \
        "$launchd_cache_inode" "$launchd_cache_original_inode"
    xattr -cr "$temp_dir/canonical" 2>/dev/null || true
    python3 modules/ssv/repack_aa_exact.py \
        "$temp_dir/canonical" "$temp_dir/mtree.patched.archive" "$payload_size"
    /usr/bin/aa list -i "$temp_dir/mtree.patched.archive" >/dev/null

    cp "$temp_dir/mtree.im4p" "$temp_dir/mtree.patched.im4p"
    ./bin/img4 \
        -i "$temp_dir/mtree.patched.im4p" \
        -R "$temp_dir/mtree.patched.archive"
    if [[ $(stat -f '%z' "$temp_dir/mtree.patched.im4p") != \
          $(stat -f '%z' "$temp_dir/mtree.im4p") ]]; then
        echo "[!] Canonical mtree IMG4 changed size; refusing unsafe ZIP patch."
        exit 1
    fi
    python3 modules/ssv/patch_stored_zip_member.py \
        "$absolute_ipsw" "$mtree_member" "$temp_dir/mtree.patched.im4p"

    unzip -p "$absolute_ipsw" "$mtree_member" > "$temp_dir/mtree.verify.im4p"
    ./bin/img4 \
        -i "$temp_dir/mtree.verify.im4p" \
        -o "$temp_dir/mtree.verify.archive"
    /usr/bin/aa list -i "$temp_dir/mtree.verify.archive" >/dev/null
    mkdir "$temp_dir/canonical.verify"
    /usr/bin/aa extract \
        -i "$temp_dir/mtree.verify.archive" \
        -d "$temp_dir/canonical.verify"
    if ! grep -Fq "    launchd_cache_loader \\" \
            "$temp_dir/canonical.verify/mtree.txt" || \
       ! grep -Fq "    launchd_cache_loader.srr \\" \
            "$temp_dir/canonical.verify/mtree.txt" || \
       ! grep -Fq "xattrsdigest=none.0 inode=$cache_loader_inode \\" \
            "$temp_dir/canonical.verify/mtree.txt" || \
       ! grep -Fq "inode=$cache_loader_original_inode \\" \
            "$temp_dir/canonical.verify/mtree.txt" || \
       ! grep -Fq "    surreal_loader \\" \
            "$temp_dir/canonical.verify/mtree.txt" || \
       ! grep -Fq "xattrsdigest=none.0 inode=$loader_inode \\" \
            "$temp_dir/canonical.verify/mtree.txt" || \
       ! grep -Fq "    launchd.plist \\" \
            "$temp_dir/canonical.verify/mtree.txt" || \
       ! grep -Fq "    launchd.plist.srr \\" \
            "$temp_dir/canonical.verify/mtree.txt" || \
       ! grep -Fq "xattrsdigest=none.0 inode=$launchd_cache_inode \\" \
            "$temp_dir/canonical.verify/mtree.txt" || \
       ! grep -Fq "inode=$launchd_cache_original_inode \\" \
            "$temp_dir/canonical.verify/mtree.txt"; then
        echo "[!] Canonical mtree verification failed after updating the IPSW."
        exit 1
    fi

    echo "[*] Experimental SSV root_hash and canonical mtree are synchronized."
    echo "[*] This restore attempt can now be repeated."
)

ssv_toggle_skip_setup(){
    if [[ $SKIP_SETUP_DEV -eq 1 ]]; then
        SKIP_SETUP_DEV=0
        echo "[*] Experimental Skip Setup: OFF"
    else
        SKIP_SETUP_DEV=1
        echo "[*] Experimental Skip Setup: ON"
        echo "[!] This patch was tested only on A13."
        echo "[!] Device and iOS compatibility is not guaranteed."
        echo "[!] This does not bypass activation."
    fi
    read -p "Press enter to continue"
}

ssv_toggle_ssh(){
    if [[ $SSHD_DEV -eq 1 ]]; then
        SSHD_DEV=0
        echo "[*] Experimental SSV SSH patches: OFF"
    else
        SSHD_DEV=1
        echo "[*] Experimental SSV SSH patches: ON"
        echo "[!] This patch was tested only on A13."
        echo "[!] Device and iOS compatibility is not guaranteed."
        echo "[!] This customization requires two restore attempts."
        echo "[!] The first pass captures the root hash; the second completes the restore."
        echo "[!] Development credentials enabled: root / alpine"
        echo "[*] USB access after boot: ./bin/iproxy 2222 22"
    fi
    read -p "Press enter to continue"
}


ssv_restore_log_has_seal_data() {
    local restore_log="$1"

    [[ -f "$restore_log" ]] || return 1
    python3 - "$restore_log" <<'PY'
import re
import sys

with open(sys.argv[1], "rb") as restore_log:
    contents = restore_log.read().decode("utf-8", "replace")
hashes = re.findall(
    r"Invalid root hash:\s*([0-9a-fA-F\s]{64,128})"
    r"\s*\(expected:\s*([0-9a-fA-F]{64})\)",
    contents,
    re.MULTILINE,
)
inodes = re.findall(
    r"SURREALRAIN_MTREE_ANCHOR_INODES=(\d+):(\d+):(\d+):(\d+):(\d+)",
    contents,
)
valid_hash = any(len(re.sub(r"\s+", "", actual)) == 64 for actual, _ in hashes)
raise SystemExit(0 if valid_hash and inodes else 1)
PY
}

ssv_prepare_restore_artifacts() {
    ssv_set_custom_ipsw_name

    if [[ ! -f "$restoredir/$CUSTOM_IPSW_NAME" ]]; then
        echo "Restore files do not exist, making new ones"
        make_custom_ipsw_a12_ios14
    else
        echo "Restore files already exist ($CUSTOM_IPSW_NAME)"
        read -p "Would you like to make new ones? (y/n): " restorefiles_remake
        if [[ $restorefiles_remake == Y || $restorefiles_remake == y ]]; then
            rm -f "$restoredir/$CUSTOM_IPSW_NAME"
            make_custom_ipsw_a12_ios14
        fi
    fi

    [[ $SSHD_DEV -eq 1 ]] || return 0
    if ssv_restore_log_has_seal_data "$restoredir/futurerestore-last.log"; then
        echo "[*] SSHD restore pass 2/2: using seal data from the restore log."
        ssv_patch_root_hash_from_restore_log \
            "$restoredir/$CUSTOM_IPSW_NAME" \
            "$restoredir/futurerestore-last.log"
    else
        echo "[*] SSHD restore pass 1/2: capturing the device root hash."
        echo "[*] Run the same restore again after this expected failure."
    fi
}

ssv_handle_restore_success() {
    write_ssv_patch_profile
}

ssv_handle_restore_failure() {
    local restore_log="$1"

    [[ $SSHD_DEV -eq 1 ]] || return 0
    if ssv_restore_log_has_seal_data "$restore_log"; then
        echo "[*] SSHD restore pass 1/2 completed."
        echo "[*] Run the same restore again to complete pass 2/2."
    fi
}

ssv_finish_ipsw_build() {
    if [[ $SSHD_DEV -eq 1 && -f "$restoredir/futurerestore-last.log" ]]; then
        mv -f "$restoredir/futurerestore-last.log" \
            "$restoredir/futurerestore-previous.log"
    fi
}

ssv_install_seal_probes() {
    [[ $SSHD_DEV -eq 1 ]] || return 0

    local deployment_target
    deployment_target=$(ssv_deployment_target)

    echo "[*] Building the native mtree seal probe..."
    xcrun --sdk iphoneos clang \
        -arch arm64 \
        -miphoneos-version-min="$deployment_target" \
        -Os \
        -Wl,-dead_strip \
        payloads/dropbear_sshd/mtree_wrapper.c \
        -o work/surrealra1n_mtree_wrapper
    ./bin/ldid -S work/surrealra1n_mtree_wrapper

    echo "[*] Building the native APFS digest probe..."
    xcrun --sdk iphoneos clang \
        -arch arm64 \
        -miphoneos-version-min="$deployment_target" \
        -Os \
        -Wl,-dead_strip \
        payloads/dropbear_sshd/apfs_sealvolume_wrapper.c \
        -o work/surrealra1n_apfs_sealvolume_wrapper
    ./bin/ldid -S work/surrealra1n_apfs_sealvolume_wrapper

    echo "[*] Installing the native mtree seal probe..."
    ./bin/hfsplus work/ramdisk.raw extract usr/sbin/mtree work/mtree.real
    ./bin/hfsplus work/ramdisk.raw rm usr/sbin/mtree
    ./bin/hfsplus work/ramdisk.raw add work/mtree.real usr/sbin/mtree.real
    ./bin/hfsplus work/ramdisk.raw chmod 100755 usr/sbin/mtree.real
    ./bin/hfsplus work/ramdisk.raw add \
        work/surrealra1n_mtree_wrapper usr/sbin/mtree
    ./bin/hfsplus work/ramdisk.raw chmod 100755 usr/sbin/mtree

    echo "[*] Installing the native APFS digest probe..."
    ./bin/hfsplus work/ramdisk.raw extract \
        System/Library/Filesystems/apfs.fs/apfs_sealvolume \
        work/apfs_sealvolume.real
    ./bin/hfsplus work/ramdisk.raw rm \
        System/Library/Filesystems/apfs.fs/apfs_sealvolume
    ./bin/hfsplus work/ramdisk.raw add \
        work/apfs_sealvolume.real \
        System/Library/Filesystems/apfs.fs/apfs_sealvolume.real
    ./bin/hfsplus work/ramdisk.raw chmod \
        100755 System/Library/Filesystems/apfs.fs/apfs_sealvolume.real
    ./bin/hfsplus work/ramdisk.raw add \
        work/surrealra1n_apfs_sealvolume_wrapper \
        System/Library/Filesystems/apfs.fs/apfs_sealvolume
    ./bin/hfsplus work/ramdisk.raw chmod \
        100755 System/Library/Filesystems/apfs.fs/apfs_sealvolume

}

ssv_install_restore_components() {
    ssv_install_skip_setup
    ssv_install_dropbear
    ssv_install_seal_probes
}

ssv_prepare_restore_ramdisk() {
    [[ $SSHD_DEV -eq 1 ]] || return 0

    echo "[*] Preparing the full rootless SSH toolset..."
    python3 payloads/dropbear_sshd/prepare_payload.py

    local base_bytes
    local payload_bytes
    local reserve_bytes=$((64 * 1024 * 1024))
    local alignment_bytes=$((16 * 1024 * 1024))
    local target_bytes

    base_bytes=$(stat -f %z work/ramdisk.raw)
    payload_bytes=$(du -sk payloads/dropbear_sshd/rootfs | awk '{print $1 * 1024}')
    target_bytes=$((base_bytes + payload_bytes + reserve_bytes))
    target_bytes=$((
        (target_bytes + alignment_bytes - 1) /
        alignment_bytes * alignment_bytes
    ))
    echo "[*] Growing restore ramdisk to $target_bytes bytes for a $payload_bytes-byte SSH payload..."
    ./bin/hfsplus work/ramdisk.raw grow "$target_bytes"
}

ssv_install_skip_setup() {
    [[ $SKIP_SETUP_DEV -eq 1 ]] || return 0

    local deployment_target
    deployment_target=$(ssv_deployment_target)

    echo "[*] Building the native restore-time SetupDone helper..."
    xcrun --sdk iphoneos clang \
        -arch arm64 \
        -miphoneos-version-min="$deployment_target" \
        -Os \
        -Wl,-dead_strip \
        payloads/skip_setup_dev/skip_setup_dev.c \
        -o work/surrealra1n_skip_setup_dev
    ./bin/ldid -S work/surrealra1n_skip_setup_dev

    echo "[*] Installing the experimental Skip Setup launch daemon..."
    ./bin/hfsplus work/ramdisk.raw add \
        work/surrealra1n_skip_setup_dev \
        usr/local/bin/surrealra1n_skip_setup_dev
    ./bin/hfsplus work/ramdisk.raw chmod \
        100755 usr/local/bin/surrealra1n_skip_setup_dev
    ./bin/hfsplus work/ramdisk.raw add \
        payloads/skip_setup_dev/com.surrealra1n.skip-setup-dev.plist \
        System/Library/LaunchDaemons/com.surrealra1n.skip-setup-dev.plist
    ./bin/hfsplus work/ramdisk.raw chmod \
        100644 System/Library/LaunchDaemons/com.surrealra1n.skip-setup-dev.plist
}

ssv_install_dropbear() {
    [[ $SSHD_DEV -eq 1 ]] || return 0

    echo "[*] Installing Dropbear with native rootless shell lookup..."
    local rootless_dropbear_imports
    local rootless_symbol
    local system_mount
    local payload_file
    local executable_file
    local executable_path
    local deployment_target
    local -a payload_codesign_args

    deployment_target=$(ssv_deployment_target)

    rootless_dropbear_imports=$(nm -u payloads/dropbear_sshd/dropbear-rootless)
    for rootless_symbol in \
        _ie_getusershell _ie_setusershell _ie_endusershell; do
        if ! grep -Fxq "$rootless_symbol" <<< "$rootless_dropbear_imports"; then
            echo "[!] Patched Dropbear lacks $rootless_symbol."
            return 1
        fi
    done
    if grep -Fxq _getusershell <<< "$rootless_dropbear_imports"; then
        echo "[!] Patched Dropbear still imports the stock shell database."
        return 1
    fi

    cp payloads/dropbear_sshd/dropbear-rootless \
        payloads/dropbear_sshd/rootfs/var/jb/usr/sbin/dropbear
    chmod 755 payloads/dropbear_sshd/rootfs/var/jb/usr/sbin/dropbear
    python3 modules/ssv/patch_sftp_path.py \
        payloads/dropbear_sshd/rootfs/var/jb/usr/sbin/dropbear
    echo "[*] Relocating payload-local dynamic libraries into /var/jb..."
    python3 modules/ssv/relocate_payload_dylibs.py \
        payloads/dropbear_sshd/rootfs/var/jb

    echo "[*] Extracting the stock launchd cache components..."
    hdiutil attach -readonly -nobrowse -noverify -plist "$fs_dmg" \
        > work/system-mount.plist
    system_mount=$(python3 - work/system-mount.plist <<'PY'
import plistlib
import sys

with open(sys.argv[1], "rb") as plist_file:
    description = plistlib.load(plist_file)
for entity in description.get("system-entities", []):
    mount_point = entity.get("mount-point")
    if mount_point:
        print(mount_point)
        break
PY
    )
    if [[ -z "$system_mount" ]]; then
        echo "[!] Could not mount the target System volume."
        return 1
    fi
    cp "$system_mount/usr/libexec/launchd_cache_loader" \
        work/launchd_cache_loader.stock
    cp "$system_mount/System/Library/xpc/launchd.plist" \
        work/launchd.plist.stock
    hdiutil detach "$system_mount"

    echo "[*] Patching the launchd cache and its targeted signature gate..."
    python3 modules/ssv/patch_launchd_cache_loader.py \
        work/launchd_cache_loader.stock work/launchd_cache_loader.patched
    python3 modules/ssv/patch_launchd_cache.py \
        work/launchd.plist.stock work/launchd.plist.patched
    codesign -d --entitlements :- work/launchd_cache_loader.stock \
        > work/launchd_cache_loader.entitlements.plist 2>/dev/null
    codesign --force --sign - --timestamp=none \
        --identifier com.apple.launchd_cache_loader \
        --entitlements work/launchd_cache_loader.entitlements.plist \
        work/launchd_cache_loader.patched

    echo "[*] Building SurrealLoader for the cached root launch job..."
    xcrun --sdk iphoneos clang \
        -arch arm64 \
        -miphoneos-version-min="$deployment_target" \
        -Os \
        -Wl,-dead_strip \
        payloads/dropbear_sshd/surreal_loader.c \
        -o work/surreal_loader
    codesign --force --sign - --timestamp=none work/surreal_loader
    codesign --verify --strict work/launchd_cache_loader.patched
    codesign --verify --strict work/surreal_loader

    echo "[*] Repairing missing or invalid payload CodeDirectories..."
    SSHD_PAYLOAD_MACHO_FILES=()
    while IFS= read -r payload_file; do
        if ! file "$payload_file" | grep -q 'Mach-O'; then
            continue
        fi
        if ! codesign --verify --strict "$payload_file" >/dev/null 2>&1; then
            ./bin/ldid -e "$payload_file" > work/payload-entitlements.plist || true
            payload_codesign_args=(--force --sign - --timestamp=none)
            if [[ -s work/payload-entitlements.plist ]]; then
                payload_codesign_args+=(
                    --entitlements work/payload-entitlements.plist
                )
            fi
            codesign "${payload_codesign_args[@]}" "$payload_file"
        fi
        if ! codesign --verify --strict "$payload_file" >/dev/null 2>&1; then
            echo "[!] Failed to create a valid CodeDirectory for $payload_file"
            return 1
        fi
        if ! otool -l "$payload_file" | grep -q 'LC_CODE_SIGNATURE'; then
            echo "[!] Failed to create a CodeDirectory for $payload_file"
            return 1
        fi
        SSHD_PAYLOAD_MACHO_FILES+=("$payload_file")
    done < <(find payloads/dropbear_sshd/rootfs -type f -print)
    if [[ ${#SSHD_PAYLOAD_MACHO_FILES[@]} -eq 0 ]]; then
        echo "[!] The SSH payload does not contain any signed Mach-O files."
        return 1
    fi
    SSHD_PAYLOAD_MACHO_FILES+=(
        work/launchd_cache_loader.patched
        work/surreal_loader
    )
    rm -f work/payload-entitlements.plist

    echo "[*] Building the restore-time Dropbear installer..."
    xcrun --sdk iphoneos clang \
        -arch arm64 \
        -miphoneos-version-min="$deployment_target" \
        -Os \
        -Wl,-dead_strip \
        payloads/dropbear_sshd/install_dropbear.c \
        -o work/surrealra1n_install_dropbear
    ./bin/ldid -S work/surrealra1n_install_dropbear

    echo "[*] Embedding the persistent SSH payload in the restore ramdisk..."
    ./bin/hfsplus work/ramdisk.raw mkdir usr/local/share
    ./bin/hfsplus work/ramdisk.raw mkdir usr/local/share/surrealra1n_dropbear
    ./bin/hfsplus work/ramdisk.raw mkdir usr/local/share/surrealra1n_dropbear/rootfs
    ./bin/hfsplus work/ramdisk.raw addall \
        payloads/dropbear_sshd/rootfs \
        usr/local/share/surrealra1n_dropbear/rootfs
    while IFS= read -r executable_file; do
        executable_path=${executable_file#payloads/dropbear_sshd/rootfs/}
        ./bin/hfsplus work/ramdisk.raw chmod \
            100755 "usr/local/share/surrealra1n_dropbear/rootfs/$executable_path"
    done < <(find payloads/dropbear_sshd/rootfs -type f -perm -111 -print)
    ./bin/hfsplus work/ramdisk.raw add \
        work/launchd_cache_loader.patched \
        usr/local/share/surrealra1n_dropbear/launchd-cache-loader
    ./bin/hfsplus work/ramdisk.raw chmod \
        100755 usr/local/share/surrealra1n_dropbear/launchd-cache-loader
    ./bin/hfsplus work/ramdisk.raw add \
        work/launchd.plist.patched \
        usr/local/share/surrealra1n_dropbear/launchd.plist
    ./bin/hfsplus work/ramdisk.raw chmod \
        100644 usr/local/share/surrealra1n_dropbear/launchd.plist
    ./bin/hfsplus work/ramdisk.raw add \
        work/surreal_loader \
        usr/local/share/surrealra1n_dropbear/surreal-loader
    ./bin/hfsplus work/ramdisk.raw chmod \
        100755 usr/local/share/surrealra1n_dropbear/surreal-loader
    ./bin/hfsplus work/ramdisk.raw add \
        work/surrealra1n_install_dropbear \
        usr/local/bin/surrealra1n_install_dropbear
    ./bin/hfsplus work/ramdisk.raw chmod \
        100755 usr/local/bin/surrealra1n_install_dropbear
    ./bin/hfsplus work/ramdisk.raw add \
        payloads/dropbear_sshd/com.surrealra1n.install-dropbear.plist \
        System/Library/LaunchDaemons/com.surrealra1n.install-dropbear.plist
    ./bin/hfsplus work/ramdisk.raw chmod \
        100644 System/Library/LaunchDaemons/com.surrealra1n.install-dropbear.plist
}

ssv_patch_restore_trustcache() {
    if [[ ! -f work/trustcache.raw ]]; then
        if [[ $SKIP_SETUP_DEV -ne 1 && $SSHD_DEV -ne 1 ]]; then
            return 0
        fi
        ./bin/img4 \
            -i "tmp1/Firmware/$ramdisk_dmg_name.trustcache" \
            -o work/trustcache.raw
    fi

    if [[ $SKIP_SETUP_DEV -eq 1 ]]; then
        ./bin/trustcache append \
            work/trustcache.raw work/surrealra1n_skip_setup_dev
    fi
    if [[ $SSHD_DEV -eq 1 ]]; then
        ./bin/trustcache append \
            work/trustcache.raw work/surrealra1n_install_dropbear
        ./bin/trustcache append \
            work/trustcache.raw work/surrealra1n_mtree_wrapper
        ./bin/trustcache append \
            work/trustcache.raw work/surrealra1n_apfs_sealvolume_wrapper
        ./bin/trustcache append \
            work/trustcache.raw "${SSHD_PAYLOAD_MACHO_FILES[@]}"
    fi
    ./bin/img4 \
        -i work/trustcache.raw \
        -o "tmp2/Firmware/$ramdisk_dmg_name_18.trustcache" \
        -A -T rtsc
}

ssv_patch_static_trustcache() {
    [[ $SSHD_DEV -eq 1 ]] || return 0

    echo "[*] Building a normalized StaticTrustCache for the SSH runtime..."
    ./bin/img4 \
        -i "tmp1/Firmware/$fs_dmg_name.trustcache" \
        -o work/rootfs-trustcache.raw
    ./bin/trustcache create -v 1 \
        work/sshd-payload-trustcache.raw "${SSHD_PAYLOAD_MACHO_FILES[@]}"
    python3 modules/ssv/normalize_trustcache.py \
        work/rootfs-trustcache.raw \
        --require work/sshd-payload-trustcache.raw
    cp "tmp1/Firmware/$fs_dmg_name.trustcache" \
        "tmp2/Firmware/$fs_dmg_18_name.trustcache"
    ./bin/img4 \
        -i "tmp2/Firmware/$fs_dmg_18_name.trustcache" \
        -R work/rootfs-trustcache.raw

    local trustcache_type
    trustcache_type=$(./bin/img4 \
        -i "tmp2/Firmware/$fs_dmg_18_name.trustcache" \
        -o work/rootfs-trustcache.verify.raw 2>&1)
    if [[ "$trustcache_type" != "trst" ]] || \
       ! cmp -s work/rootfs-trustcache.raw work/rootfs-trustcache.verify.raw; then
        echo "[!] StaticTrustCache IMG4 type or payload verification failed."
        return 1
    fi
}
