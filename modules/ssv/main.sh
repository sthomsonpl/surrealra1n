#!/bin/bash

SSV_CONFIG_PATH=${SSV_CONFIG_PATH:-boot/ssv_config.json}
SSV_PATCHED=0
SSV_CONFIG_SKIP_SETUP=0
SSV_CONFIG_SSHD=0
SSV_CONFIG_CUSTOM_BINPATCHES=0
SKIP_SETUP_DEV=0
SSHD_DEV=0
CUSTOM_BINPATCHES_DEV=0
CUSTOM_BINPATCHER_DIR=${CUSTOM_BINPATCHER_DIR:-"$SCRIPT_DIR/custom_binpatcher"}
CUSTOM_BINPATCHER_CONFIG=${CUSTOM_BINPATCHER_CONFIG:-"$CUSTOM_BINPATCHER_DIR/patches.json"}
CUSTOM_BINPATCHER_PATCH_DIR=${CUSTOM_BINPATCHER_PATCH_DIR:-"$CUSTOM_BINPATCHER_DIR/patches"}
SYSTEM_VOLUME_MODE=${SYSTEM_VOLUME_MODE:-unknown}

ssv_reset_options() {
    ssv_apply_config
}

ssv_apply_config() {
    if [[ $SSV_PATCHED -eq 1 ]]; then
        SKIP_SETUP_DEV=$SSV_CONFIG_SKIP_SETUP
        SSHD_DEV=$SSV_CONFIG_SSHD
        CUSTOM_BINPATCHES_DEV=$SSV_CONFIG_CUSTOM_BINPATCHES
    else
        SKIP_SETUP_DEV=0
        SSHD_DEV=0
        CUSTOM_BINPATCHES_DEV=0
    fi
}

ssv_write_config() {
    local config_directory
    config_directory=$(dirname "$SSV_CONFIG_PATH")
    mkdir -p "$config_directory"
    python3 - "$SSV_CONFIG_PATH" "$SSV_PATCHED" \
        "$SSV_CONFIG_SKIP_SETUP" "$SSV_CONFIG_SSHD" \
        "$SSV_CONFIG_CUSTOM_BINPATCHES" <<'PY'
import json
import os
import sys

path, patched, skip_setup, sshd, custom_binpatches = sys.argv[1:]
config = {
    "schema_version": 1,
    "ssv_patched": patched == "1",
    "patches": {
        "skip_setup": skip_setup == "1",
        "dropbear_sshd": sshd == "1",
        "custom_binpatches": custom_binpatches == "1",
    },
}
temporary = f"{path}.surrealra1n"
with open(temporary, "w", encoding="utf-8") as output:
    json.dump(config, output, indent=2, sort_keys=True)
    output.write("\n")
    output.flush()
    os.fsync(output.fileno())
os.replace(temporary, path)
PY
}

ssv_load_config() {
    local loaded_config

    if [[ ! -f "$SSV_CONFIG_PATH" ]]; then
        ssv_apply_config
        ssv_write_config
        return
    fi

    if ! loaded_config=$(python3 - "$SSV_CONFIG_PATH" 2>/dev/null <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as source:
    config = json.load(source)

if config.get("schema_version") != 1:
    raise ValueError("unsupported schema_version")

patched = config.get("ssv_patched")
patches = config.get("patches")
if not isinstance(patched, bool) or not isinstance(patches, dict):
    raise ValueError("invalid System Patches configuration")

skip_setup = patches.get("skip_setup")
sshd = patches.get("dropbear_sshd")
custom_binpatches = patches.get("custom_binpatches", False)
if (
    not isinstance(skip_setup, bool)
    or not isinstance(sshd, bool)
    or not isinstance(custom_binpatches, bool)
):
    raise ValueError("invalid patch configuration")

print(int(patched), int(skip_setup), int(sshd), int(custom_binpatches))
PY
    ); then
        echo "[!] Invalid System Patches configuration in $SSV_CONFIG_PATH; all System Patches are disabled."
        ssv_apply_config
        return
    fi

    read -r SSV_PATCHED SSV_CONFIG_SKIP_SETUP SSV_CONFIG_SSHD \
        SSV_CONFIG_CUSTOM_BINPATCHES <<< "$loaded_config"
    ssv_apply_config
}

ssv_deployment_target() {
    local major=${VERSION%%.*}
    if [[ $major =~ ^[0-9]+$ ]]; then
        printf '%s.0\n' "$major"
    else
        printf '15.0\n'
    fi
}

ssv_ios_major() {
    local major=${VERSION%%.*}
    [[ $major =~ ^[0-9]+$ ]] || return 1
    printf '%s\n' "$major"
}

ssv_ios15_features_are_supported() {
    local major
    major=$(ssv_ios_major) || return 1
    [[ $major -ge 15 && $SYSTEM_VOLUME_MODE == sealed ]]
}

ssv_detect_system_volume_mode() {
    local ipsw_path="$1"
    python3 "$SCRIPT_DIR/modules/ssv/detect_system_volume.py" "$ipsw_path"
}

ssv_configure_runtime_for_volume() {
    local mode="$1"

    case "$mode" in
        sealed|unsealed) SYSTEM_VOLUME_MODE="$mode" ;;
        *)
            echo "[!] Unsupported System volume mode: $mode"
            return 1
            ;;
    esac

    # Start from the persisted choices before applying per-target capabilities.
    ssv_apply_config
    if ! ssv_ios15_features_are_supported; then
        if [[ $SKIP_SETUP_DEV -eq 1 ]]; then
            echo "[!] Skip Setup requires a sealed System Volume on iOS 15+."
            echo "[*] Skip Setup is disabled for this restore."
        fi
        if [[ $SSHD_DEV -eq 1 ]]; then
            echo "[!] SSH/Dropbear requires a sealed System Volume on iOS 15+."
            echo "[*] SSH/Dropbear is disabled for this restore."
        fi
        SKIP_SETUP_DEV=0
        SSHD_DEV=0
    fi
}

ssv_prepare_runtime_for_ipsw() {
    local ipsw_path="$1"
    local detected_mode

    if ! detected_mode=$(ssv_detect_system_volume_mode "$ipsw_path"); then
        echo "[!] Could not determine the target System volume format."
        return 1
    fi
    ssv_configure_runtime_for_volume "$detected_mode"
    if [[ $SYSTEM_VOLUME_MODE == sealed ]]; then
        echo "[*] Detected sealed System Volume (root_hash + canonical mtree)."
    else
        echo "[*] Detected unsealed System Volume."
        echo "[*] Seal synchronization and canonical mtree patching are not required."
    fi
}

ssv_print_menu_options() {
    if [[ $SSV_PATCHED -eq 1 ]]; then
        echo "4. System Patches [ON]"
    else
        echo "4. System Patches [OFF]"
    fi
    echo "5. System Patches Config"
}

ssv_select_menu_option() {
    case "$1" in
        4) ssv_toggle_patched ;;
        5) ssv_config_menu ;;
        *) return 1 ;;
    esac
}

ssv_toggle_patched() {
    if [[ $SSV_PATCHED -eq 1 ]]; then
        SSV_PATCHED=0
        echo "[*] System Patches: OFF"
    else
        SSV_PATCHED=1
        echo "[*] System Patches: ON"
    fi
    ssv_apply_config
    ssv_write_config
    read -p "Press enter to continue"
}

ssv_config_menu() {
    local ssv_config_option
    while true; do
        clear
        echo "System Patches Configuration:"
        echo ""
        if [[ $SSV_PATCHED -eq 0 ]]; then
            echo "System Patches are OFF. These settings are saved but currently inactive."
            echo ""
        fi
        if [[ $SSV_CONFIG_SKIP_SETUP -eq 1 ]]; then
            echo "1. Skip Setup (iOS 15+; activation required) [ON]"
        else
            echo "1. Skip Setup (iOS 15+; activation required) [OFF]"
        fi
        if [[ $SSV_CONFIG_SSHD -eq 1 ]]; then
            echo "2. SSH patches (iOS 15+) [ON]"
        else
            echo "2. SSH patches (iOS 15+) [OFF]"
        fi
        if [[ $SSV_CONFIG_CUSTOM_BINPATCHES -eq 1 ]]; then
            echo "3. Custom Binpatches [ON]"
        else
            echo "3. Custom Binpatches [OFF]"
        fi
        echo "4. Custom Binpatches Configurator"
        echo "5. Back"
        read -p "Please input an option (1-5, or C): " ssv_config_option
        case "$ssv_config_option" in
            1) ssv_toggle_skip_setup ;;
            2) ssv_toggle_ssh ;;
            3) ssv_toggle_custom_binpatches ;;
            4|C|c) ssv_custom_binpatches_configurator ;;
            5) return ;;
            *)
                echo "Invalid option."
                read -p "Press enter to continue"
                ;;
        esac
    done
}

ssv_set_custom_ipsw_name() {
    CUSTOM_IPSW_NAME="customssvpatched_${VERSION}_${IDENTIFIER}.ipsw"
    SSV_IPSW_STATE_PATH="$restoredir/${CUSTOM_IPSW_NAME%.ipsw}.config.sha256"
}

ssv_current_ipsw_fingerprint() {
    local custom_active=0
    if ssv_custom_binpatches_are_active; then
        custom_active=1
    fi
    python3 - "$IDENTIFIER" "$VERSION" "$BUILD" \
        "$SKIP_SETUP_DEV" "$SSHD_DEV" "$custom_active" \
        "$CUSTOM_BINPATCHER_CONFIG" "$CUSTOM_BINPATCHER_PATCH_DIR" \
        "$SYSTEM_VOLUME_MODE" \
        "$SCRIPT_DIR/patchers/arm64e_iboot_patcher.c" <<'PY'
import glob
import hashlib
import json
import os
import sys

(
    identifier,
    ios,
    build,
    skip_setup,
    sshd,
    custom_active,
    config_path,
    patch_dir,
    volume_mode,
    arm64e_iboot_patcher_path,
) = sys.argv[1:]

enabled_definitions = {}
if custom_active == "1":
    with open(config_path, encoding="utf-8") as source:
        config = json.load(source)
    enabled_ids = {
        patch_id for patch_id, enabled in config.items() if enabled is True
    }
    for path in sorted(glob.glob(os.path.join(patch_dir, "*.json"))):
        if os.path.basename(path) == "template_patch.json":
            continue
        with open(path, encoding="utf-8") as source:
            patch = json.load(source)
        if patch.get("id") in enabled_ids:
            enabled_definitions[patch["id"]] = patch

with open(arm64e_iboot_patcher_path, "rb") as source:
    arm64e_iboot_patcher_hash = hashlib.sha256(source.read()).hexdigest()

state = {
    "identifier": identifier,
    "ios": ios,
    "build": build,
    "system_volume_mode": volume_mode,
    "skip_setup": skip_setup == "1",
    "dropbear_sshd": sshd == "1",
    "custom_binpatches": enabled_definitions,
    "arm64e_iboot_patcher": arm64e_iboot_patcher_hash,
}
serialized = json.dumps(
    state, sort_keys=True, separators=(",", ":"), ensure_ascii=True
).encode()
print(hashlib.sha256(serialized).hexdigest())
PY
}

ssv_ipsw_matches_current_config() {
    local expected
    local stored

    [[ -f "$restoredir/$CUSTOM_IPSW_NAME" && \
       -f "$SSV_IPSW_STATE_PATH" ]] || return 1
    expected=$(ssv_current_ipsw_fingerprint)
    read -r stored < "$SSV_IPSW_STATE_PATH"
    [[ "$stored" == "$expected" ]]
}

ssv_write_ipsw_fingerprint() {
    local fingerprint
    local temporary="${SSV_IPSW_STATE_PATH}.surrealra1n"

    fingerprint=$(ssv_current_ipsw_fingerprint)
    printf '%s\n' "$fingerprint" > "$temporary"
    mv "$temporary" "$SSV_IPSW_STATE_PATH"
}

write_ssv_patch_profile() {
    local profile_directory="boot/profiles/$ECID"
    local profile_path="$profile_directory/$VERSION.json"
    local custom_binpatches=0
    if ssv_custom_binpatches_are_active; then
        custom_binpatches=1
    fi
    mkdir -p "$profile_directory"
    python3 - "$profile_path" "$ECID" "$IDENTIFIER" "$VERSION" \
        "$SKIP_SETUP_DEV" "$SSHD_DEV" "$custom_binpatches" \
        "$CUSTOM_BINPATCHER_CONFIG" "$SYSTEM_VOLUME_MODE" <<'PY'
import json
import os
import sys

(
    path,
    ecid,
    identifier,
    version,
    skip_setup,
    sshd,
    custom_binpatches,
    custom_config_path,
    volume_mode,
) = sys.argv[1:]
skip_setup_enabled = skip_setup == "1"
loader_enabled = sshd == "1"
custom_enabled = custom_binpatches == "1"
custom_patch_ids = []
if custom_enabled:
    with open(custom_config_path, "r", encoding="utf-8") as source:
        custom_config = json.load(source)
    custom_patch_ids = sorted(
        patch_id for patch_id, enabled in custom_config.items() if enabled is True
    )
patch_profile = {
    "enabled": skip_setup_enabled or loader_enabled or custom_enabled,
    "skip_setup": skip_setup_enabled,
    "dropbear_sshd": loader_enabled,
    "custom_binpatches": {
        "enabled": custom_enabled,
        "patch_ids": custom_patch_ids,
    },
    "experimental": True,
}
profile = {
    "schema_version": 1,
    "ecid": ecid,
    "identifier": identifier,
    "ios_version": version,
    "system_volume_mode": volume_mode,
    "system_patches": patch_profile,
    # Retained for compatibility with existing profile consumers.
    "ssv_patches": patch_profile,
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
    echo "[*] Saved experimental System Patches profile: $profile_path"
}

ssv_patch_root_hash_from_restore_log()(
    # Sync the device seal with the IPSW metadata.
    set -euo pipefail

    local ipsw_path="$1"
    local restore_log="$2"
    local require_anchor="${3:-1}"
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

    cleanup_ssv_root_hash() {
        if [[ -n "$temp_dir" && -d "$temp_dir" ]]; then
            rm -rf "$temp_dir"
        fi
    }
    trap cleanup_ssv_root_hash EXIT

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
if matches:
    print(*matches[-1], *(inode_matches[-1] if inode_matches else ()))
PY
    )
    if [[ -z "$hash_line" ]]; then
        return 0
    fi
    read -r actual_hash expected_hash cache_loader_inode \
        cache_loader_original_inode loader_inode launchd_cache_inode \
        launchd_cache_original_inode <<< "$hash_line"

    if [[ $require_anchor -eq 1 && -z "$cache_loader_inode" ]]; then
        echo "[!] APFS reported a new root hash, but the SurrealLoader inode report is missing."
        echo "[!] Refusing to patch only half of the authenticated metadata pair."
        exit 1
    fi

    if [[ ! "$actual_hash" =~ ^[0-9a-f]{64}$ || \
          ! "$expected_hash" =~ ^[0-9a-f]{64}$ ]]; then
        echo "[!] Could not parse the root hash from $restore_log."
        exit 1
    fi
    if [[ $require_anchor -eq 1 ]]; then
        if [[ ! "$cache_loader_inode" =~ ^[1-9][0-9]*$ || \
              ! "$cache_loader_original_inode" =~ ^[1-9][0-9]*$ || \
              ! "$loader_inode" =~ ^[1-9][0-9]*$ || \
              ! "$launchd_cache_inode" =~ ^[1-9][0-9]*$ || \
              ! "$launchd_cache_original_inode" =~ ^[1-9][0-9]*$ ]]; then
            echo "[!] Could not parse the SurrealLoader inode set from $restore_log."
            exit 1
        fi
        printf -v anchor_inodes '%s:%s:%s:%s:%s' \
            "$cache_loader_inode" "$cache_loader_original_inode" "$loader_inode" \
            "$launchd_cache_inode" "$launchd_cache_original_inode"
    fi

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
    if [[ $require_anchor -eq 1 ]]; then
        echo "[*] Device reported the SurrealLoader anchor inodes: $anchor_inodes"
    fi
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

    if [[ $require_anchor -ne 1 ]]; then
        echo "[*] Experimental sealed System root_hash is synchronized."
        echo "[*] This restore attempt can now be repeated."
        exit 0
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

    echo "[*] Experimental sealed System root_hash and canonical mtree are synchronized."
    echo "[*] This restore attempt can now be repeated."
)

ssv_toggle_skip_setup(){
    if [[ $SSV_CONFIG_SKIP_SETUP -eq 1 ]]; then
        SSV_CONFIG_SKIP_SETUP=0
        echo "[*] Experimental Skip Setup: OFF"
    else
        SSV_CONFIG_SKIP_SETUP=1
        echo "[*] Experimental Skip Setup: ON"
        echo "[!] Device and iOS compatibility is not guaranteed."
        echo "[!] This does not bypass activation."
    fi
    ssv_apply_config
    ssv_write_config
    read -p "Press enter to continue"
}

ssv_toggle_ssh(){
    if [[ $SSV_CONFIG_SSHD -eq 1 ]]; then
        SSV_CONFIG_SSHD=0
        echo "[*] Experimental System SSH patches: OFF"
    else
        SSV_CONFIG_SSHD=1
        echo "[*] Experimental System SSH patches: ON"
        echo "[!] Device and iOS compatibility is not guaranteed."
        echo "[!] This customization requires two restore attempts."
        echo "[!] The first pass captures the root hash; the second completes the restore."
        echo "[!] Development credentials enabled: root / alpine"
        echo "[*] USB access after boot: ./bin/iproxy 2222 22"
    fi
    ssv_apply_config
    ssv_write_config
    read -p "Press enter to continue"
}

ssv_toggle_custom_binpatches() {
    if [[ $SSV_CONFIG_CUSTOM_BINPATCHES -eq 1 ]]; then
        SSV_CONFIG_CUSTOM_BINPATCHES=0
        echo "[*] Custom Binpatches: OFF"
    else
        SSV_CONFIG_CUSTOM_BINPATCHES=1
        echo "[*] Custom Binpatches: ON"
        echo "[!] Sealed System Volumes require two restore attempts."
        echo "[*] Unsealed System Volumes are patched in a single restore."
        echo "[!] Use option 4 to choose the individual patches."
    fi
    ssv_apply_config
    ssv_write_config
    read -p "Press enter to continue"
}

ssv_custom_binpatches_configurator() {
    if ! python3 "$CUSTOM_BINPATCHER_DIR/custom_binpatcher_configurator.py" \
            --patch-dir "$CUSTOM_BINPATCHER_PATCH_DIR" \
            --config "$CUSTOM_BINPATCHER_CONFIG"; then
        echo "[!] Custom Binpatches configurator failed."
        read -p "Press enter to continue"
    fi
}

ssv_custom_binpatches_are_active() {
    [[ $CUSTOM_BINPATCHES_DEV -eq 1 ]] || return 1
    python3 - "$CUSTOM_BINPATCHER_CONFIG" <<'PY'
import json
import sys

try:
    with open(sys.argv[1], "r", encoding="utf-8") as source:
        config = json.load(source)
    active = isinstance(config, dict) and any(value is True for value in config.values())
except (OSError, json.JSONDecodeError):
    active = False
raise SystemExit(0 if active else 1)
PY
}

ssv_validate_custom_binpatches() {
    [[ $CUSTOM_BINPATCHES_DEV -eq 1 ]] || return 0
    python3 "$CUSTOM_BINPATCHER_DIR/custom_bin_patcher.py" \
        --patch-dir "$CUSTOM_BINPATCHER_PATCH_DIR" \
        --config "$CUSTOM_BINPATCHER_CONFIG" \
        --list >/dev/null
}

ssv_requires_seal_sync() {
    [[ $SYSTEM_VOLUME_MODE == sealed ]] || return 1
    [[ $SSHD_DEV -eq 1 ]] || ssv_custom_binpatches_are_active
}

ssv_confirm_two_pass_restore() {
    ssv_requires_seal_sync || return 0
    echo ""
    echo "[!] This sealed System configuration requires two restore attempts."
    echo "[!] Pass 1 is expected to fail after capturing the new APFS root hash."
    echo "[!] Run Start Restore again and keep the existing IPSW for pass 2."
    read -p "Press enter to continue"
}


ssv_restore_log_has_seal_data() {
    local restore_log="$1"
    local require_inodes="${2:-0}"

    [[ -f "$restore_log" ]] || return 1
    python3 - "$restore_log" "$require_inodes" <<'PY'
import re
import sys

with open(sys.argv[1], "rb") as restore_log:
    contents = restore_log.read().decode("utf-8", "replace")
require_inodes = sys.argv[2] == "1"
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
raise SystemExit(0 if valid_hash and (inodes or not require_inodes) else 1)
PY
}

ssv_prepare_restore_artifacts() {
    if [[ $SYSTEM_VOLUME_MODE == unknown ]]; then
        echo "[!] System volume mode was not detected before preparing artifacts."
        return 1
    fi
    ssv_validate_custom_binpatches
    ssv_set_custom_ipsw_name

    if [[ ! -f "$restoredir/$CUSTOM_IPSW_NAME" ]]; then
        echo "Restore files do not exist, making new ones"
        make_custom_ipsw_a12_ios14
    elif ! ssv_ipsw_matches_current_config; then
        echo "[*] The saved IPSW does not match the current System Patches configuration."
        echo "[*] Rebuilding $CUSTOM_IPSW_NAME automatically..."
        rm -f "$restoredir/$CUSTOM_IPSW_NAME" "$SSV_IPSW_STATE_PATH"
        make_custom_ipsw_a12_ios14
    else
        echo "Restore files already exist ($CUSTOM_IPSW_NAME)"
        read -p "Would you like to make new ones? (y/n): " restorefiles_remake
        if [[ $restorefiles_remake == Y || $restorefiles_remake == y ]]; then
            rm -f "$restoredir/$CUSTOM_IPSW_NAME" "$SSV_IPSW_STATE_PATH"
            make_custom_ipsw_a12_ios14
        fi
    fi

    ssv_requires_seal_sync || return 0
    if ssv_restore_log_has_seal_data \
            "$restoredir/futurerestore-last.log" "$SSHD_DEV"; then
        echo "[*] Sealed System restore pass 2/2: using seal data from the restore log."
        ssv_patch_root_hash_from_restore_log \
            "$restoredir/$CUSTOM_IPSW_NAME" \
            "$restoredir/futurerestore-last.log" "$SSHD_DEV"
    else
        echo "[*] Sealed System restore pass 1/2: capturing the device root hash."
        echo "[*] Run the same restore again after this expected failure."
    fi
}

ssv_handle_restore_success() {
    write_ssv_patch_profile
}

ssv_handle_restore_failure() {
    local restore_log="$1"

    ssv_requires_seal_sync || return 0
    if ssv_restore_log_has_seal_data "$restore_log" "$SSHD_DEV"; then
        echo "[*] Sealed System restore pass 1/2 completed."
        echo "[*] Run the same restore again to complete pass 2/2."
    fi
}

ssv_finish_ipsw_build() {
    ssv_write_ipsw_fingerprint
    if ssv_requires_seal_sync && \
            [[ -f "$restoredir/futurerestore-last.log" ]]; then
        mv -f "$restoredir/futurerestore-last.log" \
            "$restoredir/futurerestore-previous.log"
    fi
}

ssv_apply_custom_binpatches() (
    set -euo pipefail
    ssv_custom_binpatches_are_active || return 0

    local source_dmg="$1"
    local output_dmg="$2"
    local mount_plist="$SCRIPT_DIR/work/custom-binpatcher-mount.plist"
    local shadow_file="$SCRIPT_DIR/work/custom-binpatcher-system.shadow"
    local merged_dmg="$SCRIPT_DIR/work/custom-binpatcher-system.dmg"
    local metadata="$SCRIPT_DIR/work/custom-binpatcher-metadata.json"
    local backup_dir="$SCRIPT_DIR/$restoredir/custom-binpatcher-backups/${BUILD:-unknown}"
    local signed_dir="$SCRIPT_DIR/work/custom-binpatcher-signed"
    local system_mount=""
    local python_bin
    python_bin=$(command -v python3)

    cleanup_custom_binpatcher_mount() {
        if [[ -n "$system_mount" ]]; then
            hdiutil detach "$system_mount" >/dev/null 2>&1 || true
        fi
        rm -f "$mount_plist" "$shadow_file" "$merged_dmg"
    }
    trap cleanup_custom_binpatcher_mount EXIT

    rm -rf "$signed_dir"
    rm -f "$metadata" "$mount_plist" "$shadow_file" "$merged_dmg"
    mkdir -p "$backup_dir" "$signed_dir"

    echo "[*] Mounting a writable shadow of the target System volume..."
    hdiutil attach -nobrowse -noverify -owners on \
        -shadow "$shadow_file" -plist "$source_dmg" > "$mount_plist"
    system_mount=$(python3 - "$mount_plist" <<'PY'
import os
import plistlib
import sys

with open(sys.argv[1], "rb") as source:
    description = plistlib.load(source)
for entity in description.get("system-entities", []):
    mount_point = entity.get("mount-point")
    if mount_point and os.path.isdir(os.path.join(mount_point, "usr")):
        print(mount_point)
        break
PY
    )
    if [[ -z "$system_mount" ]]; then
        echo "[!] Could not locate the mounted target System volume."
        return 1
    fi

    echo "[*] Applying configured Custom Binpatches..."
    sudo "$python_bin" "$CUSTOM_BINPATCHER_DIR/custom_bin_patcher.py" \
        --system-root "$system_mount" \
        --ios "$VERSION" \
        --build "$BUILD" \
        --patch-dir "$CUSTOM_BINPATCHER_PATCH_DIR" \
        --config "$CUSTOM_BINPATCHER_CONFIG" \
        --sign-tool "$SCRIPT_DIR/bin/ldid" \
        --backup-dir "$backup_dir" \
        --metadata-output "$metadata"
    sudo chown -R "$(id -u):$(id -g)" "$backup_dir" "$metadata"

    python3 - "$system_mount" "$metadata" "$signed_dir" <<'PY'
import json
import os
import shutil
import sys

system_root, metadata_path, output_root = sys.argv[1:]
with open(metadata_path, encoding="utf-8") as source:
    metadata = json.load(source)
for item in metadata:
    relative = item["target"].lstrip("/")
    source_path = os.path.join(system_root, relative)
    output_path = os.path.join(output_root, relative)
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    shutil.copyfile(source_path, output_path)
    os.chmod(output_path, os.stat(source_path).st_mode & 0o7777)
PY

    sync
    hdiutil detach "$system_mount"
    system_mount=""

    echo "[*] Materializing the patched System image..."
    hdiutil convert "$source_dmg" \
        -shadow "$shadow_file" \
        -format ULFO \
        -o "$merged_dmg"
    [[ -s "$merged_dmg" ]] || {
        echo "[!] hdiutil did not create the patched System image."
        return 1
    }
    echo "[*] Rebuilding ASR checksums for the patched System image..."
    /usr/sbin/asr imagescan \
        --source "$merged_dmg" \
        --nostream
    /usr/sbin/asr info \
        --source "$merged_dmg" \
        --plist >/dev/null
    mv "$merged_dmg" "$output_dmg"
    rm -f "$shadow_file"
    echo "[*] Custom Binpatches applied to the target System image."
)

ssv_patch_custom_canonical_mtree() {
    ssv_custom_binpatches_are_active || return 0
    [[ $SYSTEM_VOLUME_MODE == sealed ]] || return 0

    local mtree_path="$1"
    local metadata="$SCRIPT_DIR/work/custom-binpatcher-metadata.json"
    local temp_dir="$SCRIPT_DIR/work/custom-binpatcher-mtree"
    local payload_size

    [[ -f "$mtree_path" && -f "$metadata" ]] || {
        echo "[!] Custom Binpatcher mtree inputs are missing."
        return 1
    }

    rm -rf "$temp_dir"
    mkdir -p "$temp_dir/canonical" "$temp_dir/verify"
    ./bin/img4 -i "$mtree_path" -o "$temp_dir/mtree.archive"
    payload_size=$(stat_size "$temp_dir/mtree.archive")
    /usr/bin/aa extract \
        -i "$temp_dir/mtree.archive" \
        -d "$temp_dir/canonical"
    python3 modules/ssv/patch_custom_mtree.py \
        "$temp_dir/canonical/mtree.txt" "$metadata"
    xattr -cr "$temp_dir/canonical" 2>/dev/null || true
    python3 modules/ssv/repack_aa_exact.py \
        "$temp_dir/canonical" "$temp_dir/mtree.patched.archive" "$payload_size"

    cp "$mtree_path" "$temp_dir/mtree.patched.im4p"
    ./bin/img4 \
        -i "$temp_dir/mtree.patched.im4p" \
        -R "$temp_dir/mtree.patched.archive"
    if [[ $(stat_size "$temp_dir/mtree.patched.im4p") != \
          $(stat_size "$mtree_path") ]]; then
        echo "[!] Custom Binpatcher mtree IMG4 changed size."
        return 1
    fi

    ./bin/img4 \
        -i "$temp_dir/mtree.patched.im4p" \
        -o "$temp_dir/mtree.verify.archive"
    /usr/bin/aa list -i "$temp_dir/mtree.verify.archive" >/dev/null
    /usr/bin/aa extract \
        -i "$temp_dir/mtree.verify.archive" \
        -d "$temp_dir/verify"
    python3 modules/ssv/patch_custom_mtree.py \
        "$temp_dir/verify/mtree.txt" "$metadata" --verify
    mv "$temp_dir/mtree.patched.im4p" "$mtree_path"
    echo "[*] Custom Binpatcher canonical mtree inodes synchronized."
}

ssv_install_seal_probes() {
    ssv_requires_seal_sync || return 0

    local deployment_target
    deployment_target=$(ssv_deployment_target)

    if [[ $SSHD_DEV -eq 1 ]]; then
        echo "[*] Building the native mtree seal probe..."
        xcrun --sdk iphoneos clang \
            -arch arm64 \
            -miphoneos-version-min="$deployment_target" \
            -Os \
            -Wl,-dead_strip \
            payloads/dropbear_sshd/mtree_wrapper.c \
            -o work/surrealra1n_mtree_wrapper
        ./bin/ldid -S work/surrealra1n_mtree_wrapper
    fi

    echo "[*] Building the native APFS digest probe..."
    xcrun --sdk iphoneos clang \
        -arch arm64 \
        -miphoneos-version-min="$deployment_target" \
        -Os \
        -Wl,-dead_strip \
        payloads/dropbear_sshd/apfs_sealvolume_wrapper.c \
        -o work/surrealra1n_apfs_sealvolume_wrapper
    ./bin/ldid -S work/surrealra1n_apfs_sealvolume_wrapper

    if [[ $SSHD_DEV -eq 1 ]]; then
        echo "[*] Installing the native mtree seal probe..."
        ./bin/hfsplus work/ramdisk.raw extract usr/sbin/mtree work/mtree.real
        ./bin/hfsplus work/ramdisk.raw rm usr/sbin/mtree
        ./bin/hfsplus work/ramdisk.raw add work/mtree.real usr/sbin/mtree.real
        ./bin/hfsplus work/ramdisk.raw chmod 100755 usr/sbin/mtree.real
        ./bin/hfsplus work/ramdisk.raw add \
            work/surrealra1n_mtree_wrapper usr/sbin/mtree
        ./bin/hfsplus work/ramdisk.raw chmod 100755 usr/sbin/mtree
    fi

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
    if [[ $SKIP_SETUP_DEV -ne 1 && $SSHD_DEV -ne 1 ]] && \
            ! ssv_requires_seal_sync; then
        return 0
    fi

    if [[ ! -f work/trustcache.raw ]]; then
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
            work/trustcache.raw "${SSHD_PAYLOAD_MACHO_FILES[@]}"
    fi
    if ssv_requires_seal_sync; then
        ./bin/trustcache append \
            work/trustcache.raw work/surrealra1n_apfs_sealvolume_wrapper
    fi
    ./bin/img4 \
        -i work/trustcache.raw \
        -o "tmp2/Firmware/$ramdisk_dmg_name_18.trustcache" \
        -A -T rtsc
}

ssv_patch_static_trustcache() {
    local -a required_files=()
    local custom_file
    if [[ $SSHD_DEV -eq 1 ]]; then
        required_files+=("${SSHD_PAYLOAD_MACHO_FILES[@]}")
    fi
    if ssv_custom_binpatches_are_active; then
        while IFS= read -r custom_file; do
            required_files+=("$custom_file")
        done < <(find work/custom-binpatcher-signed -type f -print)
    fi
    [[ ${#required_files[@]} -gt 0 ]] || return 0

    echo "[*] Building a normalized StaticTrustCache for System customizations..."
    ./bin/img4 \
        -i "tmp1/Firmware/$fs_dmg_name.trustcache" \
        -o work/rootfs-trustcache.raw
    ./bin/trustcache create -v 1 \
        work/ssv-required-trustcache.raw "${required_files[@]}"
    python3 modules/ssv/normalize_trustcache.py \
        work/rootfs-trustcache.raw \
        --require work/ssv-required-trustcache.raw
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
