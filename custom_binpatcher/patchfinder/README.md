# Custom Binpatcher Patchfinder

`custombin_patchfinder` is a standalone thin Mach-O arm64/arm64e analyzer. It
resolves a function symbol, Objective-C method, or function referencing a
C string and finds exactly one matching ARM64 instruction. It never modifies
the input binary.

It has no external library dependencies.

## Analyze a known instruction

`--gen` performs reverse lookup for a known file offset. It validates the
current instruction bytes, identifies the containing Objective-C method or
function symbol, and reports both the exact instruction and a generalized
matcher. It only returns analysis information; it does not generate a complete
patch definition.

```sh
../../bin/custombin_patchfinder \
  --gen \
  --input /path/to/exampled \
  --offset 0x16918 \
  --expected "00 48 40 39"
```

Example output:

```json
{
  "status": "analyzed",
  "file_offset": "0x16918",
  "virtual_address": "0x100016918",
  "expected": "00 48 40 39",
  "expected_matches": true,
  "owner": {
    "kind": "objc_method",
    "class": "ExampleClass",
    "selector": "exampleSelector",
    "method_kind": "instance",
    "implementation": "0x100016918",
    "instruction_offset": "0x0"
  },
  "decoded_instruction": {
    "mnemonic": "LDRB",
    "destination": "W0",
    "memory": {
      "base": "X0",
      "offset": "0x12"
    }
  },
  "generalized_instruction": {
    "mnemonic": "LDRB",
    "destination": "W0",
    "memory": {
      "base": "X0",
      "offset": "*"
    }
  }
}
```

Memory displacements are generalized to `"*"`; register constraints remain
exact. If the supplied bytes do not match, the tool returns exit code `1` and
a JSON object containing `expected` and `actual`.

## Save output

Use `--output` to write the JSON result to a file instead of standard output:

```sh
../../bin/custombin_patchfinder \
  --gen \
  --input /path/to/exampled \
  --offset 0x16918 \
  --expected "00 48 40 39" \
  --output exampled-analysis.json
```

The file is overwritten if it already exists. For human-readable indentation,
pipe the result through `jq`:

```sh
../../bin/custombin_patchfinder ... | jq . > exampled-analysis.json
```

## Build

From this directory:

```sh
make
```

The output is written to:

```text
../../bin/custombin_patchfinder
```

Equivalent direct compilation:

```sh
cc -std=c11 -O2 -Wall -Wextra \
  custombin_patchfinder.c -o custombin_patchfinder
```

## Objective-C method example

```sh
../../bin/custombin_patchfinder \
  --input /path/to/exampled \
  --objc-class ExampleClass \
  --selector exampleSelector \
  --method-kind instance \
  --mnemonic LDRB \
  --destination W0 \
  --base X0 \
  --offset '*'
```

`--method-kind` accepts `instance` (the default) or `class`.

## Function symbol example

```sh
../../bin/custombin_patchfinder \
  --input /path/to/exampled \
  --function _example_function \
  --mnemonic MOV \
  --destination W0 \
  --immediate '*'
```

## C-string XREF example

Use `--cstring-xref` for stripped binaries without a stable symbol or
Objective-C owner:

```sh
../../bin/custombin_patchfinder \
  --input /path/to/amfid \
  --cstring-xref security.mac.amfi.developer_mode_status \
  --mnemonic CSET \
  --destination W0 \
  --condition EQ
```

The string must resolve to exactly one referencing function. Function bounds
come from `LC_FUNCTION_STARTS`; direct `ADR` references and `ADRP` plus `ADD`
materialization are supported. Ambiguous strings or instruction matches fail
without returning a patch offset.

## Matcher options

Supported instruction mnemonics:

```text
LDRB LDR STR MOV CMP CSET CBZ CBNZ TBZ TBNZ B BL RET
```

Optional constraints:

```text
--destination W0|W?|X0|X?
--source W0|W?|X0|X?
--base X0|X?
--offset VALUE|*
--immediate VALUE|*
--condition EQ|NE|CS|CC|MI|PL|VS|VC|HI|LS|GE|LT|GT|LE|*
```

Exact register numbers from 0 through 31 are accepted. Numeric values may be
decimal or use a prefix understood by C, such as `0x11`.

## Output

On success, the program writes one compact JSON object to standard output:

```json
{"status":"found","file_offset":"0x16918","virtual_address":"0x100016918","size":4,"bytes":"00 48 40 39","matches":1}
```

Fields:

- `status`: `found` on success;
- `file_offset`: offset suitable for byte patching the Mach-O file;
- `virtual_address`: unslid instruction address;
- `size`: matched byte count;
- `bytes`: current instruction bytes in file order;
- `matches`: always `1` on successful unique matching.

Example with `jq`:

```sh
../../bin/custombin_patchfinder ... | jq -r '.file_offset'
```

Errors are written to standard error. Exit codes are:

- `0`: exactly one match was found;
- `1`: parsing, resolution, or matching failed;
- `2`: command-line arguments are invalid.
