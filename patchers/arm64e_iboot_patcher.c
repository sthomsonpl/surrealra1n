#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long addr_t;

#define PACIBSP 0xD503237FU
#define RETAB   0xD65F0FFFU
#define MOV_X0_0 0xD2800000U

static uint32_t read32(const uint8_t *buf, size_t off)
{
    uint32_t value;
    memcpy(&value, buf + off, sizeof(value));
    return value;
}

static void write32(uint8_t *buf, size_t off, uint32_t value)
{
    memcpy(buf + off, &value, sizeof(value));
}

static addr_t xref64(const uint8_t *buf, addr_t start, addr_t end, addr_t what)
{
    addr_t i;
    uint64_t value[32];
    memset(value, 0, sizeof(value));

    end &= ~3ULL;
    for (i = start & ~3ULL; i < end; i += 4) {
        uint32_t op = read32(buf, (size_t)i);
        unsigned reg = op & 0x1F;

        if ((op & 0x9F000000) == 0x90000000) {
            signed adr = ((op & 0x60000000) >> 18) |
                         ((op & 0xFFFFE0) << 8);
            value[reg] = ((long long)adr << 1) + (i & ~0xFFFULL);
            continue;
        }
        if ((op & 0xFF000000) == 0x91000000) {
            unsigned rn = (op >> 5) & 0x1F;
            unsigned shift = (op >> 22) & 3;
            unsigned imm = (op >> 10) & 0xFFF;
            if (shift == 1)
                imm <<= 12;
            else if (shift > 1)
                continue;
            value[reg] = value[rn] + imm;
        } else if ((op & 0xF9C00000) == 0xF9400000) {
            unsigned rn = (op >> 5) & 0x1F;
            unsigned imm = ((op >> 10) & 0xFFF) << 3;
            if (!imm)
                continue;
            value[reg] = value[rn] + imm;
        } else if ((op & 0x9F000000) == 0x10000000) {
            signed adr = ((op & 0x60000000) >> 18) |
                         ((op & 0xFFFFE0) << 8);
            value[reg] = ((long long)adr >> 11) + i;
        } else if ((op & 0xFF000000) == 0x58000000) {
            unsigned adr = (op & 0xFFFFE0) >> 3;
            value[reg] = adr + i;
        }

        if (value[reg] == what)
            return i;
    }
    return 0;
}

static uint32_t *find_next_insn(
    uint32_t *from, uint32_t num, uint32_t insn, uint32_t mask)
{
    while (num--) {
        if ((*from & mask) == (insn & mask))
            return from;
        from++;
    }
    return NULL;
}

static uint32_t *find_prev_insn(
    uint32_t *from, uint32_t num, uint32_t insn, uint32_t mask)
{
    while (num--) {
        if ((*from & mask) == (insn & mask))
            return from;
        from--;
    }
    return NULL;
}

static int patch_ios15_property_callback(uint8_t *buf, size_t len)
{
    const char *needle = "Unknown ASN1 type %llu\n";
    void *str_ref = memmem(buf, len, needle, strlen(needle));
    uint32_t *ret_insn;
    uint32_t *mov_insn;
    addr_t str_off;
    addr_t xref;

    if (!str_ref)
        return 1;

    printf("[+] iOS 15+ property callback anchor @ buf+0x%lx\n",
           (uintptr_t)str_ref - (uintptr_t)buf);

    str_off = (uintptr_t)str_ref - (uintptr_t)buf;
    xref = xref64(buf, 0, len, str_off);
    if (!xref) {
        printf("[-] no xref to property callback anchor\n");
        return -1;
    }

    ret_insn = find_next_insn(
        (uint32_t *)(buf + xref), 0x300, 0xD65F03C0, 0xFFFFFFFF);
    if (!ret_insn) {
        printf("[-] no RET found after property callback anchor\n");
        return -1;
    }

    mov_insn = find_prev_insn(
        ret_insn, 0x100, 0xAA0003E0, 0xFF0000FF);
    if (!mov_insn) {
        printf("[-] no MOV X0, Xn found before property callback RET\n");
        return -1;
    }

    printf("[+] iOS 15+ return patch @ buf+0x%lx\n",
           (uintptr_t)mov_insn - (uintptr_t)buf);
    *mov_insn = MOV_X0_0;
    return 0;
}

static int is_sub_sp_sp(uint32_t op)
{
    return (op & 0xFFC003FFU) == 0xD10003FFU;
}

static int is_mov_x0_xn(uint32_t op)
{
    return (op & 0xFF0000FFU) == 0xAA0000E0U;
}

static int is_ldp_fp_lr_from_sp(uint32_t op)
{
    return (op & 0xFFC003FFU) == 0xA94003FDU &&
           ((op >> 10) & 0x1F) == 30;
}

static int find_unique_u32(
    const uint8_t *buf, size_t len, uint32_t needle, size_t *match)
{
    size_t i;
    unsigned count = 0;

    for (i = 0; i + sizeof(uint32_t) <= len; i += 4) {
        if (read32(buf, i) == needle) {
            *match = i;
            count++;
        }
    }
    return count == 1 ? 0 : -1;
}

static int has_iboot_6723(const uint8_t *buf, size_t len)
{
    return memmem(buf, len, "iBoot-6723.", strlen("iBoot-6723.")) != NULL;
}

static int patch_ios14_property_callback(uint8_t *buf, size_t len)
{
    /*
     * Unique instruction in the CSEC manifest-property dispatch case:
     *     MOVK W0, #0x4353, LSL #16
     *
     * The iOS 14 callback is substantially larger than its iOS 15 successor.
     * Locate and validate the complete PAC function before patching its single
     * return-value move. Never use fixed per-build offsets.
     */
    const uint32_t csec_movk = 0x72A86A60U;
    size_t anchor;
    size_t prologue = 0;
    size_t retab = 0;
    size_t return_mov = 0;
    size_t pos;

    if (!has_iboot_6723(buf, len))
        return 1;

    if (find_unique_u32(buf, len, csec_movk, &anchor) != 0) {
        printf("[-] expected exactly one iBoot 6723 CSEC anchor\n");
        return -1;
    }

    pos = anchor;
    while (pos >= 8 && anchor - pos <= 0x1000) {
        if (read32(buf, pos - 4) == PACIBSP &&
            is_sub_sp_sp(read32(buf, pos))) {
            prologue = pos - 4;
            break;
        }
        pos -= 4;
    }
    if (!prologue) {
        printf("[-] validated iBoot 6723 PACIBSP prologue not found\n");
        return -1;
    }

    for (pos = anchor; pos + 4 <= len && pos - anchor <= 0x1000;
         pos += 4) {
        if (pos != prologue && read32(buf, pos) == PACIBSP) {
            printf("[-] crossed into another PAC function before RETAB\n");
            return -1;
        }
        if (read32(buf, pos) == RETAB) {
            retab = pos;
            break;
        }
    }
    if (!retab) {
        printf("[-] iBoot 6723 property callback RETAB not found\n");
        return -1;
    }

    for (pos = retab; pos >= prologue + 8 && retab - pos <= 0x80;
         pos -= 4) {
        uint32_t op = read32(buf, pos - 4);
        if (is_mov_x0_xn(op)) {
            return_mov = pos - 4;
            break;
        }
    }
    if (!return_mov ||
        !is_ldp_fp_lr_from_sp(read32(buf, return_mov + 4))) {
        printf("[-] validated iBoot 6723 return epilogue not found\n");
        return -1;
    }

    printf("[+] iBoot 6723 CSEC anchor @ buf+0x%zx\n", anchor);
    printf("[+] iBoot 6723 PAC function @ buf+0x%zx\n", prologue);
    printf("[+] iBoot 6723 return patch @ buf+0x%zx\n", return_mov);
    write32(buf, return_mov, MOV_X0_0);
    return 0;
}

static int patch_iboot_signature_check(uint8_t *buf, size_t len)
{
    int result;

    if (has_iboot_6723(buf, len))
        return patch_ios14_property_callback(buf, len);

    result = patch_ios15_property_callback(buf, len);
    if (result <= 0)
        return result;

    printf("[-] unsupported arm64e iBoot property callback\n");
    return -1;
}

int main(int argc, char *argv[])
{
    FILE *fp;
    uint8_t *buf;
    size_t len;

    printf("surrealra1n arm64e iBoot signature patcher\n");

    if (argc != 3) {
        printf("usage: %s <iBoot.in> <iBoot.patched>\n", argv[0]);
        return 1;
    }

    fp = fopen(argv[1], "rb");
    if (!fp) {
        perror(argv[1]);
        return 1;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        perror("fseek");
        fclose(fp);
        return 1;
    }
    len = (size_t)ftell(fp);
    if (fseek(fp, 0, SEEK_SET) != 0) {
        perror("fseek");
        fclose(fp);
        return 1;
    }

    buf = malloc(len);
    if (!buf) {
        printf("[-] malloc failed\n");
        fclose(fp);
        return 1;
    }
    if (fread(buf, 1, len, fp) != len) {
        printf("[-] read failed\n");
        fclose(fp);
        free(buf);
        return 1;
    }
    fclose(fp);

    printf("[*] loaded %zu bytes from %s\n", len, argv[1]);
    if (patch_iboot_signature_check(buf, len) != 0) {
        printf("[-] patching failed\n");
        free(buf);
        return 1;
    }

    fp = fopen(argv[2], "wb");
    if (!fp) {
        perror(argv[2]);
        free(buf);
        return 1;
    }
    if (fwrite(buf, 1, len, fp) != len) {
        printf("[-] write failed\n");
        fclose(fp);
        free(buf);
        return 1;
    }
    if (fclose(fp) != 0) {
        perror("fclose");
        free(buf);
        return 1;
    }

    free(buf);
    printf("[+] written to %s\n", argv[2]);
    return 0;
}
