# Custom Binpatcher

Custom Binpatcher applies user-defined byte patches to binaries in an unpacked
iOS System volume. It validates every expected byte before changing anything,
combines all operations targeting the same binary, signs each changed binary
once, creates a backup, and replaces the original file atomically.

Only Python 3 and the tools already bundled with Surrealra1nForge are required.

## Private patch files

Patch definitions and their enabled states are local files and are intentionally
ignored by Git:

- `patches/*.json`, except tracked templates and bundled patch definitions
- `patches.json`

The template is documentation and is skipped by both the patcher and the
configurator. Copy it to create a real patch:

```sh
cp custom_binpatcher/patches/template_patch.json \
  custom_binpatcher/patches/my-patch.json
```

For a patch that changes more than one binary, start from
`template_multi_target_patch.json` instead.
For instruction-based discovery, use `template_patchfind.json`.

Edit the copied file, then run the configurator:

```sh
python3 custom_binpatcher/custom_binpatcher_configurator.py
```

New patches start as `OFF`. Toggle the required entries and select `Save`.
Saving creates `custom_binpatcher/patches.json` if it does not exist.

The same configurator is available from:

```text
Restore menu → System Patches Config → Custom Binpatches Configurator
```

The `Custom Binpatches` master switch and the individual patch switch must both
be enabled before Surrealra1nForge applies a patch.

The generated IPSW uses one predictable name:

```text
customssvpatched_<iOS version>_<device identifier>.ipsw
```

For example:

```text
customssvpatched_15.6.1_iPhone12,5.ipsw
```

Skip Setup, SSH, and all enabled Custom Binpatches are combined in this one
image. A configuration fingerprint is stored beside it. Changing a System
Patches option,
an individual patch state, or the contents of an enabled patch automatically
rebuilds the IPSW. For sealed targets, the second restore pass reuses the same
image when its configuration has not changed.

## Patch format

Each patch definition is one JSON object. The original single-target format is:

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

### Multi-target patches

For one logical patch that changes multiple binaries, define stable target aliases
at the top level and select an alias on every operation:

```json
{
  "id": "example-multi-target-patch",
  "name": "Example multi-target binary patch",
  "description": "Replace this description with a concise explanation of the patch.",
  "targets": {
    "daemon-a": "/usr/libexec/exampled-a",
    "daemon-b": "/usr/libexec/exampled-b"
  },
  "versions": [
    {
      "ios": "15.6.1",
      "build": "19G82",
      "operations": [
        {
          "target": "daemon-a",
          "offset": "0x1234",
          "expected": "00 00 80 52",
          "replace": "20 00 80 52"
        },
        {
          "target": "daemon-b",
          "offset": "0x5678",
          "expected": "00 00 80 52",
          "replace": "20 00 80 52"
        }
      ]
    }
  ]
}
```

- Use either top-level `target` (single-target) or `targets` (multi-target), not both.
- `targets` maps a non-empty alias to an absolute path inside the iOS System volume.
- Every multi-target operation must contain `target`, whose value is a declared alias.
- Version selectors apply to the patch as a whole; target paths do not need to be repeated per version.
- Existing single-target definitions retain their current format and behavior.

### Patchfind patches

`patchfind` is an alternative to fixed file offsets. It is always a list and may
be declared directly on a patch or inside a version variant. The compiled
`custombin_patchfinder` resolves an Objective-C method, function symbol, or a
function referencing an exact C string, matches one ARM64 instruction
semantically, and returns its current file offset and bytes. `replace` remains
a fixed hexadecimal byte string.

The standalone source, build instructions, CLI reference, and output schema are
documented in [`patchfinder/README.md`](patchfinder/README.md).
The standalone `--gen` mode can also reverse-map a known file offset and byte
sequence to its containing Objective-C method/function and generalized
instruction matcher.

Use `"function": "_symbol_name"` instead of `objc_method` for a Mach-O
function symbol. Objective-C methods accept an optional `"kind": "class"`;
the default is `"instance"`. For stripped binaries, use
`"cstring_xref": "exact string"`. C-string anchors use `LC_FUNCTION_STARTS`
and support both direct `ADR` and `ADRP` plus `ADD` references.

Instruction constraints currently support:

- mnemonics `LDRB`, `LDR`, `STR`, `MOV`, `CMP`, `CSET`, `CBZ`, `CBNZ`, `TBZ`,
  `TBNZ`, `B`, `BL`, and `RET`;
- exact registers such as `W0` and `X10`, or same-width wildcards `W?` and
  `X?`;
- `destination`, `source`, `immediate`, `condition`, and
  `memory.base`/`memory.offset`;
- `"*"` for any immediate or memory displacement.

Only `"match": "unique"` is supported. Zero or multiple matches stop the
patch before any target is changed. The matched bytes become the internal
`expected` value, so they are validated again immediately before patching.
The replacement must have the same length as the matched instruction.

For multi-target patchfind definitions, select the top-level target alias on
each finder:

```json
{
  "targets": {
    "daemon-a": "/usr/libexec/exampled-a",
    "daemon-b": "/usr/libexec/exampled-b"
  },
  "patchfind": [
    {
      "target": "daemon-a",
      "function": "_first_function",
      "instruction": { "mnemonic": "MOV", "destination": "W0", "immediate": "*" },
      "replace": "20 00 80 52"
    },
    {
      "target": "daemon-b",
      "function": "_second_function",
      "instruction": { "mnemonic": "LDRB", "destination": "W0", "memory": { "base": "X?", "offset": "*" } },
      "replace": "20 00 80 52"
    }
  ]
}
```

`operations` and `patchfind` may coexist inside the same selected version.
Top-level patchfind entries are version-independent; `versions[].patchfind`
uses the existing iOS/build selector rules.

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

Multiple operations may be included in one variant, including operations for
different aliases in a multi-target patch. Multiple enabled patches may also target
the same binary. The patcher groups them by resolved target and then:

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
--patchfinder-tool PATH custombin_patchfinder-compatible executable
--backup-dir PATH      Store backups outside the System volume
--metadata-output PATH Write changed target paths and inode numbers
--dry-run              Validate and report without changing files
--list                 List available patches and their state
```

## System volume restore workflow

When active custom patches are used, Surrealra1nForge detects the System volume mode
from `BuildManifest.plist`, mounts a writable shadow of the System image,
applies and signs the configured patches, updates StaticTrustCache,
materializes the modified image, and rebuilds its ASR checksums.

For a sealed System Volume, Surrealra1nForge also updates canonical mtree inode
entries. The sealed workflow requires two restore attempts:

1. the first attempt captures the APFS seal hash and is expected to fail;
2. start the restore again, keep the existing IPSW, and let Surrealra1nForge update
   its root hash metadata before the second attempt.

An unsealed System Volume skips root hash, canonical mtree, and seal-probe
handling and completes in one restore attempt.

Surrealra1nForge does not rebuild snapshots or root hashes inside Custom Binpatcher
itself; those steps remain part of the surrounding sealed System workflow.
