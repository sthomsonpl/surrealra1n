/*
 * restored_external patcher.
 *
 * Original patchfinder and restored patcher work by @mineekdev.
 * Extended for seprmvr64 v2 and Cryptex by @pwnerblu.
 * Forge integration makes the patch set composable and fail closed:
 * no output is written unless every requested patch is found and applied.
 */
#define _GNU_SOURCE

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t addr_t;

#define MOV_X0_ZERO 0xd2800000U
#define RET         0xd65f03c0U

enum patch_flag {
    PATCH_SEP      = 1U << 0,
    PATCH_FDR      = 1U << 1,
    PATCH_BASEBAND = 1U << 2,
    PATCH_CRYPTEX  = 1U << 3,
};

#define PATCH_STANDARD (PATCH_SEP | PATCH_FDR | PATCH_BASEBAND)
#define PATCH_IOS17    PATCH_CRYPTEX

static uint32_t read32(const uint8_t *buffer, size_t offset)
{
    uint32_t value;
    memcpy(&value, buffer + offset, sizeof(value));
    return value;
}

static void write32(uint8_t *buffer, size_t offset, uint32_t value)
{
    memcpy(buffer + offset, &value, sizeof(value));
}

static int find_unique_data(const uint8_t *buffer, size_t length,
                            const char *needle, size_t *result)
{
    const size_t needle_length = strlen(needle);
    size_t matches = 0;

    if (!needle_length || needle_length > length) {
        return -1;
    }
    for (size_t offset = 0; offset <= length - needle_length; offset++) {
        if (memcmp(buffer + offset, needle, needle_length) == 0) {
            *result = offset;
            matches++;
        }
    }
    if (matches != 1) {
        fprintf(stderr, "[-] Expected one '%s' string, found %zu\n",
                needle, matches);
        return -1;
    }
    return 0;
}

/* Minimal arm64 ADR/ADRP xref tracker inherited from patchfinder64. */
static int find_xrefs(const uint8_t *buffer, size_t length, addr_t target,
                      size_t *results, size_t capacity, size_t *result_count)
{
    uint64_t values[32] = {0};
    bool valid[32] = {false};
    size_t matches = 0;
    const size_t end = length & ~(size_t)3;

    for (size_t offset = 0; offset + 4 <= end; offset += 4) {
        const uint32_t operation = read32(buffer, offset);
        const unsigned reg = operation & 0x1f;
        bool is_reference = false;

        if ((operation & 0x9f000000U) == 0x90000000U) {
            const int32_t address = (int32_t)(
                ((operation & 0x60000000U) >> 18) |
                ((operation & 0x00ffffe0U) << 8));
            values[reg] = (uint64_t)(((int64_t)address << 1) +
                                     (int64_t)(offset & ~(size_t)0xfff));
            valid[reg] = true;
            continue;
        }
        if ((operation & 0xff000000U) == 0x91000000U) {
            const unsigned source = (operation >> 5) & 0x1f;
            const unsigned shift = (operation >> 22) & 3;
            unsigned immediate = (operation >> 10) & 0xfff;
            if (shift == 1) {
                immediate <<= 12;
            } else if (shift > 1) {
                continue;
            }
            if (!valid[source]) {
                valid[reg] = false;
                continue;
            }
            values[reg] = values[source] + immediate;
            valid[reg] = true;
            is_reference = true;
        } else if ((operation & 0xf9c00000U) == 0xf9400000U) {
            const unsigned source = (operation >> 5) & 0x1f;
            const unsigned immediate = ((operation >> 10) & 0xfff) << 3;
            if (!immediate || !valid[source]) {
                continue;
            }
            values[reg] = values[source] + immediate;
            valid[reg] = true;
            is_reference = true;
        } else if ((operation & 0x9f000000U) == 0x10000000U) {
            const int32_t address = (int32_t)(
                ((operation & 0x60000000U) >> 18) |
                ((operation & 0x00ffffe0U) << 8));
            values[reg] = (uint64_t)(((int64_t)address >> 11) +
                                     (int64_t)offset);
            valid[reg] = true;
            is_reference = true;
        } else if ((operation & 0xff000000U) == 0x58000000U) {
            const unsigned address = (operation & 0x00ffffe0U) >> 3;
            values[reg] = address + offset;
            valid[reg] = true;
            is_reference = true;
        }

        if (is_reference && valid[reg] && values[reg] == target) {
            if (matches >= capacity) {
                fprintf(stderr, "[-] Too many xrefs to 0x%llx\n",
                        (unsigned long long)target);
                return -1;
            }
            results[matches] = offset;
            matches++;
        }
    }
    if (!matches) {
        fprintf(stderr, "[-] No xrefs to 0x%llx\n",
                (unsigned long long)target);
        return -1;
    }
    *result_count = matches;
    return 0;
}

static int find_return_result(const uint8_t *buffer, size_t length,
                              size_t xref, size_t *result)
{
    size_t return_offset = 0;

    size_t scan_end = xref + (0x300U * 4U);
    if (scan_end < xref || scan_end > length) {
        scan_end = length;
    }
    for (size_t offset = xref & ~(size_t)3;
         offset + 4 <= scan_end; offset += 4) {
        if (read32(buffer, offset) == RET) {
            return_offset = offset;
            break;
        }
    }
    if (!return_offset) {
        return -1;
    }

    const size_t lower = return_offset > (0x100U * 4U) ?
                         return_offset - (0x100U * 4U) : 0;
    for (size_t offset = return_offset; offset >= lower + 4; offset -= 4) {
        const size_t candidate = offset - 4;
        const uint32_t operation = read32(buffer, candidate);
        const uint32_t next = candidate + 8 <= length ?
                              read32(buffer, candidate + 4) : 0;
        /* Refuse a second pass instead of selecting an earlier unrelated MOV. */
        if (operation == MOV_X0_ZERO &&
            (next & 0xffc003ffU) == 0x910003ffU) {
            return -2;
        }
        /* MOV X0, Xn is the ORR alias used for the function result. */
        if ((operation & 0xff00001fU) == 0xaa000000U) {
            *result = candidate;
            return 0;
        }
    }
    return -1;
}

static int patch_return_zero(uint8_t *buffer, size_t length,
                             const char *marker, const char *label)
{
    size_t string_offset;
    size_t xrefs[64];
    size_t xref_count;
    size_t patch_offset = 0;

    printf("[*] Locating %s\n", label);
    if (find_unique_data(buffer, length, marker, &string_offset) != 0 ||
        find_xrefs(buffer, length, string_offset, xrefs,
                   sizeof(xrefs) / sizeof(xrefs[0]), &xref_count) != 0) {
        return -1;
    }

    for (size_t index = 0; index < xref_count; index++) {
        size_t candidate;
        const int find_result = find_return_result(
            buffer, length, xrefs[index], &candidate);
        if (find_result == -2) {
            fprintf(stderr, "[-] %s is already patched\n", label);
            return -1;
        }
        if (find_result != 0) {
            fprintf(stderr, "[-] No result MOV after xref 0x%zx for %s\n",
                    xrefs[index], marker);
            return -1;
        }
        printf("[*] %s xref 0x%zx resolves to result 0x%zx\n",
               label, xrefs[index], candidate);
        if (!patch_offset) {
            patch_offset = candidate;
        } else if (patch_offset != candidate) {
            fprintf(stderr,
                    "[-] %s xrefs resolve to different patch points\n",
                    marker);
            return -1;
        }
    }

    const uint32_t operation = read32(buffer, patch_offset);
    printf("[+] Patching %s result at 0x%zx: %08x -> %08x\n",
           label, patch_offset, operation, MOV_X0_ZERO);
    write32(buffer, patch_offset, MOV_X0_ZERO);
    return 0;
}

static int patch_fdr(uint8_t *buffer, size_t length)
{
    return patch_return_zero(buffer, length, "RestoredFDRRecover",
                             "RestoredFDRRecover");
}

static int patch_sep(uint8_t *buffer, size_t length)
{
    return patch_return_zero(buffer, length, "AppleSEPManager",
                             "_ramrod_device_has_sep");
}

static int patch_cryptex(uint8_t *buffer, size_t length)
{
    return patch_return_zero(buffer, length,
                             "_get_and_save_large_file_internal",
                             "Cryptex install validation");
}

static int find_function_start(const uint8_t *buffer, size_t start,
                               size_t where, size_t *result)
{
    where &= ~(size_t)3;
    start &= ~(size_t)3;
    for (size_t cursor = where; cursor >= start; cursor -= 4) {
        const uint32_t operation = read32(buffer, cursor);
        if ((operation & 0xffc003ffU) == 0x910003fdU) {
            const unsigned delta = (operation >> 10) & 0xfff;
            if ((delta & 0xf) == 0) {
                const size_t distance = ((delta >> 4) + 1U) * 4U;
                if (cursor >= start + distance) {
                    const size_t previous = cursor - distance;
                    if ((read32(buffer, previous) & 0xffc003e0U) ==
                        0xa98003e0U) {
                        *result = previous;
                        return 0;
                    }
                }

                size_t previous = cursor;
                while (previous >= start + 4) {
                    previous -= 4;
                    const uint32_t candidate = read32(buffer, previous);
                    if ((candidate & 0xffc003ffU) == 0xd10003ffU &&
                        ((candidate >> 10) & 0xfff) == delta + 0x10) {
                        *result = previous;
                        return 0;
                    }
                    if ((candidate & 0xffc003e0U) != 0xa90003e0U) {
                        break;
                    }
                }
            }
        }
        if (cursor < start + 4) {
            break;
        }
    }
    return -1;
}

static int patch_baseband(uint8_t *buffer, size_t length)
{
    const char *marker = "%s: querying baseband info";
    size_t string_offset;
    size_t xrefs[8];
    size_t xref_count;
    size_t function_start;

    printf("[*] Locating _ramrod_device_has_baseband\n");
    if (find_unique_data(buffer, length, marker, &string_offset) != 0 ||
        find_xrefs(buffer, length, string_offset, xrefs,
                   sizeof(xrefs) / sizeof(xrefs[0]), &xref_count) != 0 ||
        xref_count != 1 ||
        find_function_start(buffer, 0, xrefs[0], &function_start) != 0) {
        fprintf(stderr, "[-] Failed to locate baseband function\n");
        return -1;
    }

    size_t end = xrefs[0] + 0x100U;
    if (end < xrefs[0] || end > length) {
        end = length;
    }
    for (size_t offset = function_start; offset + 4 <= end; offset += 4) {
        const uint32_t operation = read32(buffer, offset);
        if ((operation & 0x7f000000U) == 0x34000000U) {
            int32_t immediate = (int32_t)((operation >> 5) & 0x7ffff);
            if (immediate & 0x40000) {
                immediate |= ~0x7ffff;
            }
            const int64_t target = (int64_t)offset + ((int64_t)immediate << 2);
            const int64_t branch_immediate = (target - (int64_t)offset) >> 2;
            if (branch_immediate < -(1LL << 25) ||
                branch_immediate >= (1LL << 25)) {
                fprintf(stderr, "[-] Baseband branch target is out of range\n");
                return -1;
            }
            const uint32_t replacement = 0x14000000U |
                ((uint32_t)branch_immediate & 0x03ffffffU);
            printf("[+] Patching baseband branch at 0x%zx: %08x -> %08x\n",
                   offset, operation, replacement);
            write32(buffer, offset, replacement);
            return 0;
        }
    }
    fprintf(stderr, "[-] Failed to find baseband CBZ/CBNZ\n");
    return -1;
}

static int apply_patches(uint8_t *buffer, size_t length, unsigned patches)
{
    if ((patches & PATCH_SEP) && patch_sep(buffer, length) != 0) {
        return -1;
    }
    if ((patches & PATCH_FDR) && patch_fdr(buffer, length) != 0) {
        return -1;
    }
    if ((patches & PATCH_BASEBAND) && patch_baseband(buffer, length) != 0) {
        return -1;
    }
    if ((patches & PATCH_CRYPTEX) && patch_cryptex(buffer, length) != 0) {
        return -1;
    }
    return 0;
}

static int write_output(const char *path, const uint8_t *buffer, size_t length)
{
    FILE *file = fopen(path, "wb");
    if (!file) {
        perror(path);
        return -1;
    }
    int failed = fwrite(buffer, 1, length, file) != length;
    if (fflush(file) != 0) {
        failed = 1;
    }
    if (fclose(file) != 0) {
        failed = 1;
    }
    if (failed) {
        fprintf(stderr, "[-] Failed writing %s\n", path);
        remove(path);
        return -1;
    }
    return 0;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s <restored_external> <output> [patch ...]\n"
            "presets:\n"
            "  --standard  patch SEP, FDR and baseband checks (default)\n"
            "  --ios17     patch Cryptex install validation\n"
            "individual patches:\n"
            "  --sep       patch the SEP presence check\n"
            "  --fdr       patch the FDR recovery check\n"
            "  --baseband  patch the baseband presence check\n"
            "  --cryptex   patch Cryptex install validation\n"
            "compatibility aliases:\n"
            "  -b          equivalent to --fdr --baseband\n"
            "  -c          equivalent to --cryptex\n",
            program);
}

static int parse_patches(int argc, char **argv, unsigned *patches)
{
    if (argc == 3) {
        *patches = PATCH_STANDARD;
        return 0;
    }

    *patches = 0;
    for (int index = 3; index < argc; index++) {
        if (strcmp(argv[index], "--standard") == 0) {
            *patches |= PATCH_STANDARD;
        } else if (strcmp(argv[index], "--ios17") == 0) {
            *patches |= PATCH_IOS17;
        } else if (strcmp(argv[index], "--sep") == 0) {
            *patches |= PATCH_SEP;
        } else if (strcmp(argv[index], "--fdr") == 0) {
            *patches |= PATCH_FDR;
        } else if (strcmp(argv[index], "--baseband") == 0) {
            *patches |= PATCH_BASEBAND;
        } else if (strcmp(argv[index], "--cryptex") == 0 ||
                   strcmp(argv[index], "-c") == 0) {
            *patches |= PATCH_CRYPTEX;
        } else if (strcmp(argv[index], "-b") == 0) {
            *patches |= PATCH_FDR | PATCH_BASEBAND;
        } else {
            fprintf(stderr, "[-] Unknown option: %s\n", argv[index]);
            return -1;
        }
    }
    return *patches ? 0 : -1;
}

int main(int argc, char **argv)
{
    unsigned patches;

    printf("restored patcher by @mineekdev\n");
    printf("modified by @pwnerblu for seprmvr64 v2 and Cryptex\n");
    printf("fail-closed Forge integration by @sthomsonpl\n");

    if (argc < 3) {
        usage(argv[0]);
        return 2;
    }
    if (strcmp(argv[1], argv[2]) == 0) {
        fprintf(stderr, "[-] Input and output paths must be different\n");
        return 2;
    }
    if (parse_patches(argc, argv, &patches) != 0) {
        usage(argv[0]);
        return 2;
    }

    FILE *file = fopen(argv[1], "rb");
    if (!file) {
        perror(argv[1]);
        return 1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 1;
    }
    const long file_length = ftell(file);
    if (file_length <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 1;
    }

    const size_t length = (size_t)file_length;
    uint8_t *buffer = malloc(length);
    if (!buffer || fread(buffer, 1, length, file) != length) {
        fprintf(stderr, "[-] Failed reading %s\n", argv[1]);
        free(buffer);
        fclose(file);
        return 1;
    }
    fclose(file);

    if (apply_patches(buffer, length, patches) != 0) {
        free(buffer);
        return 1;
    }
    if (write_output(argv[2], buffer, length) != 0) {
        free(buffer);
        return 1;
    }
    free(buffer);
    printf("[+] Patched file written to %s\n", argv[2]);
    return 0;
}
