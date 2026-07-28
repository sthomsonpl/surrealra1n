# Custom Binpatcher

Custom Binpatcher applies user-defined byte patches to binaries in an unpacked
iOS System volume. It validates every expected byte before changing anything,
combines all operations targeting the same binary, signs each changed binary
once, creates a backup, and replaces the original file atomically.

Only Python 3 and the tools already bundled with surrealra1n are required.

## Private patch files

Patch definitions and their enabled states are local files and are intentionally
ignored by Git:

- `patches/*.json`, except `patches/template_patch.json`
- `patches.json`

The template is documentation and is skipped by both the patcher and the
configurator. Copy it to create a real patch:

```sh
cp custom_binpatcher/patches/template_patch.json \
  custom_binpatcher/patches/my-patch.json
```

Edit the copied file, then run the configurator:

```sh
python3 custom_binpatcher/custom_binpatcher_configurator.py
```

New patches start as `OFF`. Toggle the required entries and select `Save`.
Saving creates `custom_binpatcher/patches.json` if it does not exist.

The same configurator is available from:

```text
Restore menu → SSV Config → Custom Binpatches Configurator
```

The `Custom Binpatches` master switch and the individual patch switch must both
be enabled before surrealra1n applies a patch.

The generated IPSW uses one predictable name:

```text
customssvpatched_<iOS version>_<device identifier>.ipsw
```

For example:

```text
customssvpatched_15.6.1_iPhone12,5.ipsw
```

Skip Setup, SSH, and all enabled Custom Binpatches are combined in this one
image. A configuration fingerprint is stored beside it. Changing an SSV option,
an individual patch state, or the contents of an enabled patch automatically
rebuilds the IPSW. The second SSV restore pass reuses the same image when its
configuration has not changed.

## Patch format

Each patch definition is one JSON object:

```json
{
  "id": "example-custom-patch",
  "name": "Example custom binary patch",
  "description": "Explain what the patch changes.",
  "target": "/usr/libexec/exampled",
  "versions": [
    {
      "ios": "15.6.1",
      "build": "19G82",
      "operations": [
        {
          "offset": "0x1234",
          "expected": "00 00 80 52",
          "replace": "20 00 80 52"
        }
      ]
    }
  ]
}
```

- `id` must be unique across all patch files.
- `name` and `description` are displayed by the configurator.
- `target` is an absolute path inside the original iOS System volume.
- `versions` contains one or more selectors and their operations.
- `offset` is a file offset, not a virtual address.
- `expected` and `replace` are hexadecimal byte strings of equal length.
- Hexadecimal strings may contain spaces or be written without spaces.

Do not leave example selectors or the example `default` variant in a real patch
unless they have verified offsets and bytes.

## Version selectors

The template demonstrates every supported selector:

- exact iOS and build: `"ios": "15.6.1", "build": "19G82"`
- exact build: `"build": "19G82"`
- exact iOS: `"ios": "15.6.1"`
- iOS wildcard: `"ios": "15.4.*"`
- build wildcard: `"build": "19G*"`
- bounded range: `"ios_min": "15.1", "ios_max": "15.4.1"`
- wildcard upper range: `"ios_min": "15.1", "ios_max": "15.4.*"`
- fallback: `"default": true`

Ranges are inclusive. For example, `ios_max: "15.4.1"` excludes iOS 15.4.2,
while `ios_max: "15.4.*"` includes every iOS 15.4 patch release.

Variants are selected in this order:

1. exact iOS and build
2. exact build
3. exact iOS
4. matching range
5. wildcard
6. default

If multiple variants have the same priority, the first one listed in `versions`
wins. A variant cannot combine `ios` with `ios_min` or `ios_max`.

## Operations and safety

Multiple operations may be included in one variant. Multiple enabled patches
may also target the same binary. The patcher groups them by target and then:

1. reads the original binary into memory;
2. validates every offset and every `expected` byte;
3. rejects overlapping operations;
4. applies all replacements to an in-memory copy;
5. signs the changed binary with `ldid`, preserving its entitlements;
6. preserves permissions, owner, group, timestamps, flags, and extended
   attributes;
7. creates a `.bak` backup if one does not already exist;
8. atomically replaces the original binary.

Any validation or signing failure stops the operation before originals are
replaced.

## Direct CLI usage

```sh
python3 custom_binpatcher/custom_bin_patcher.py \
  --system-root ./work/rootfs \
  --ios 15.6.1 \
  --build 19G82
```

Useful options:

```text
--patch-dir PATH       Patch definition directory
--config PATH          Enabled/disabled patch state file
--sign-tool PATH       ldid-compatible signing tool
--backup-dir PATH      Store backups outside the System volume
--metadata-output PATH Write changed target paths and inode numbers
--dry-run              Validate and report without changing files
--list                 List available patches and their state
```

## SSV restore workflow

When active custom patches are used, surrealra1n mounts a writable shadow of
the System image, applies and signs the configured patches, updates the
canonical mtree inode entries and StaticTrustCache, materializes the modified
image, and rebuilds its ASR checksums.

This SSV workflow requires two restore attempts:

1. the first attempt captures the APFS seal hash and is expected to fail;
2. start the restore again, keep the existing IPSW, and let surrealra1n update
   its root hash metadata before the second attempt.

surrealra1n does not rebuild snapshots or root hashes inside Custom Binpatcher
itself; those steps remain part of the surrounding SSV restore workflow.
