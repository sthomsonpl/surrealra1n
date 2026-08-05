#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MH_MAGIC_64 0xFEEDFACFU
#define CPU_TYPE_ARM64 0x0100000CU
#define LC_SEGMENT_64 0x19U
#define LC_SYMTAB 0x2U
#define LC_FUNCTION_STARTS 0x26U
#define SECTION_TYPE 0x000000FFU
#define S_CSTRING_LITERALS 0x2U
#define S_ATTR_PURE_INSTRUCTIONS 0x80000000U
#define S_ATTR_SOME_INSTRUCTIONS 0x00000400U
#define MAX_SECTIONS 128
#define MAX_NAME 17

typedef struct {
    uint32_t magic;
    int32_t cputype;
    int32_t cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
    uint32_t reserved;
} mach_header_64_t;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
} load_command_t;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    char segname[16];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    int32_t maxprot;
    int32_t initprot;
    uint32_t nsects;
    uint32_t flags;
} segment_command_64_t;

typedef struct {
    char sectname[16];
    char segname[16];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
} section_64_t;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t symoff;
    uint32_t nsyms;
    uint32_t stroff;
    uint32_t strsize;
} symtab_command_t;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t dataoff;
    uint32_t datasize;
} linkedit_data_command_t;

typedef struct {
    uint32_t n_strx;
    uint8_t n_type;
    uint8_t n_sect;
    uint16_t n_desc;
    uint64_t n_value;
} nlist_64_t;

typedef struct {
    char sectname[MAX_NAME];
    char segname[MAX_NAME];
    uint64_t addr;
    uint64_t size;
    uint64_t offset;
    uint32_t flags;
} section_info_t;

typedef struct {
    uint8_t *data;
    size_t size;
    uint64_t image_base;
    section_info_t sections[MAX_SECTIONS];
    size_t section_count;
    symtab_command_t symtab;
    int has_symtab;
    linkedit_data_command_t function_starts;
    int has_function_starts;
} macho_t;

typedef struct {
    const char *input;
    int generate;
    const char *output;
    const char *function;
    const char *cstring_xref;
    const char *objc_class;
    const char *selector;
    const char *method_kind;
    const char *mnemonic;
    const char *destination;
    const char *source;
    const char *base;
    const char *offset;
    const char *immediate;
    const char *condition;
    const char *expected;
} options_t;

typedef struct {
    const char *mnemonic;
    int destination;
    char destination_class;
    int source;
    char source_class;
    int base;
    int has_offset;
    uint64_t offset;
    int has_immediate;
    uint64_t immediate;
    int condition;
} decoded_t;

typedef struct {
    int found;
    int is_objc;
    int class_method;
    uint64_t implementation;
    char class_name[256];
    char selector[256];
    char function[256];
} owner_t;

static FILE *json_stream;

static uint32_t read_u32(const uint8_t *p)
{
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static int32_t read_i32(const uint8_t *p)
{
    int32_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static uint64_t read_u64(const uint8_t *p)
{
    uint64_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static int range_ok(size_t size, uint64_t offset, uint64_t length)
{
    return offset <= size && length <= size - offset;
}

static void copy_name(char output[MAX_NAME], const char input[16])
{
    memcpy(output, input, 16);
    output[16] = '\0';
}

static int load_file(const char *path, uint8_t **data, size_t *size)
{
    FILE *file = fopen(path, "rb");
    long length;

    if (!file) {
        fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "cannot size %s\n", path);
        fclose(file);
        return -1;
    }
    *data = malloc((size_t)length);
    if (!*data || fread(*data, 1, (size_t)length, file) != (size_t)length) {
        fprintf(stderr, "cannot read %s\n", path);
        free(*data);
        *data = NULL;
        fclose(file);
        return -1;
    }
    fclose(file);
    *size = (size_t)length;
    return 0;
}

static int parse_macho(macho_t *macho, const char *path)
{
    const mach_header_64_t *header;
    uint64_t cursor;
    uint32_t index;

    memset(macho, 0, sizeof(*macho));
    if (load_file(path, &macho->data, &macho->size) != 0)
        return -1;
    if (!range_ok(macho->size, 0, sizeof(*header))) {
        fprintf(stderr, "input is too small for a Mach-O header\n");
        return -1;
    }
    header = (const mach_header_64_t *)macho->data;
    if (header->magic != MH_MAGIC_64 ||
        (uint32_t)header->cputype != CPU_TYPE_ARM64) {
        fprintf(stderr, "only thin arm64/arm64e Mach-O files are supported\n");
        return -1;
    }
    cursor = sizeof(*header);
    for (index = 0; index < header->ncmds; index++) {
        const load_command_t *command;

        if (!range_ok(macho->size, cursor, sizeof(*command))) {
            fprintf(stderr, "truncated Mach-O load commands\n");
            return -1;
        }
        command = (const load_command_t *)(macho->data + cursor);
        if (command->cmdsize < sizeof(*command) ||
            !range_ok(macho->size, cursor, command->cmdsize)) {
            fprintf(stderr, "invalid Mach-O load command size\n");
            return -1;
        }
        if (command->cmd == LC_SEGMENT_64) {
            const segment_command_64_t *segment =
                (const segment_command_64_t *)command;
            const section_64_t *sections;
            uint32_t section_index;

            if (command->cmdsize < sizeof(*segment) +
                    (uint64_t)segment->nsects * sizeof(section_64_t)) {
                fprintf(stderr, "truncated LC_SEGMENT_64\n");
                return -1;
            }
            if (segment->fileoff == 0 &&
                (!macho->image_base || segment->vmaddr < macho->image_base))
                macho->image_base = segment->vmaddr;
            sections = (const section_64_t *)(segment + 1);
            for (section_index = 0; section_index < segment->nsects;
                 section_index++) {
                section_info_t *output;
                const section_64_t *section = &sections[section_index];

                if (macho->section_count >= MAX_SECTIONS) {
                    fprintf(stderr, "too many Mach-O sections\n");
                    return -1;
                }
                if (!range_ok(macho->size, section->offset, section->size)) {
                    fprintf(stderr, "section lies outside the input file\n");
                    return -1;
                }
                output = &macho->sections[macho->section_count++];
                copy_name(output->sectname, section->sectname);
                copy_name(output->segname, section->segname);
                output->addr = section->addr;
                output->size = section->size;
                output->offset = section->offset;
                output->flags = section->flags;
            }
        } else if (command->cmd == LC_SYMTAB) {
            if (command->cmdsize < sizeof(symtab_command_t)) {
                fprintf(stderr, "truncated LC_SYMTAB\n");
                return -1;
            }
            memcpy(&macho->symtab, command, sizeof(macho->symtab));
            macho->has_symtab = 1;
        } else if (command->cmd == LC_FUNCTION_STARTS) {
            if (command->cmdsize < sizeof(linkedit_data_command_t)) {
                fprintf(stderr, "truncated LC_FUNCTION_STARTS\n");
                return -1;
            }
            memcpy(&macho->function_starts, command,
                   sizeof(macho->function_starts));
            if (!range_ok(macho->size, macho->function_starts.dataoff,
                          macho->function_starts.datasize)) {
                fprintf(stderr, "LC_FUNCTION_STARTS lies outside the input file\n");
                return -1;
            }
            macho->has_function_starts = 1;
        }
        cursor += command->cmdsize;
    }
    if (!macho->image_base) {
        fprintf(stderr, "Mach-O image base was not found\n");
        return -1;
    }
    return 0;
}

static const section_info_t *find_section(
    const macho_t *macho, const char *segment, const char *section)
{
    size_t index;
    for (index = 0; index < macho->section_count; index++) {
        const section_info_t *item = &macho->sections[index];
        if ((!segment || strcmp(item->segname, segment) == 0) &&
            strcmp(item->sectname, section) == 0)
            return item;
    }
    return NULL;
}

static int va_to_offset(const macho_t *macho, uint64_t va, uint64_t *offset)
{
    size_t index;
    for (index = 0; index < macho->section_count; index++) {
        const section_info_t *section = &macho->sections[index];
        if (va >= section->addr && va - section->addr < section->size) {
            *offset = section->offset + (va - section->addr);
            return range_ok(macho->size, *offset, 1) ? 0 : -1;
        }
    }
    return -1;
}

static int offset_to_va(const macho_t *macho, uint64_t offset, uint64_t *va)
{
    size_t index;
    for (index = 0; index < macho->section_count; index++) {
        const section_info_t *section = &macho->sections[index];
        if (offset >= section->offset &&
            offset - section->offset < section->size) {
            *va = section->addr + (offset - section->offset);
            return 0;
        }
    }
    return -1;
}

static int valid_va(const macho_t *macho, uint64_t va)
{
    uint64_t offset;
    return va_to_offset(macho, va, &offset) == 0;
}

static int read_uleb128(
    const uint8_t *data, size_t size, size_t *cursor, uint64_t *value)
{
    uint64_t result = 0;
    unsigned shift = 0;

    while (*cursor < size && shift < 64) {
        uint8_t byte = data[(*cursor)++];
        if (shift == 63 && (byte & 0x7EU))
            return -1;
        result |= (uint64_t)(byte & 0x7FU) << shift;
        if (!(byte & 0x80U)) {
            *value = result;
            return 0;
        }
        shift += 7;
    }
    return -1;
}

static int function_range_containing(
    const macho_t *macho, uint64_t target, uint64_t *start, uint64_t *end)
{
    const section_info_t *text = find_section(macho, "__TEXT", "__text");
    const linkedit_data_command_t *command = &macho->function_starts;
    const uint8_t *data;
    size_t cursor = 0;
    uint64_t current = macho->image_base;
    uint64_t previous = 0;

    if (!text || !macho->has_function_starts ||
        !range_ok(macho->size, command->dataoff, command->datasize))
        return -1;
    data = macho->data + command->dataoff;
    while (cursor < command->datasize) {
        uint64_t delta;
        if (read_uleb128(data, command->datasize, &cursor, &delta) != 0)
            return -1;
        if (!delta)
            break;
        if (UINT64_MAX - current < delta)
            return -1;
        current += delta;
        if (previous && target >= previous && target < current) {
            *start = previous;
            *end = current;
            return 0;
        }
        previous = current;
    }
    if (previous && target >= previous && target < text->addr + text->size) {
        *start = previous;
        *end = text->addr + text->size;
        return 0;
    }
    return -1;
}

static int64_t sign_extend(uint64_t value, unsigned bits)
{
    uint64_t sign = 1ULL << (bits - 1);
    return (int64_t)((value ^ sign) - sign);
}

static int decode_adr_target(uint32_t op, uint64_t pc, uint64_t *target)
{
    uint64_t immediate;
    int64_t displacement;

    if ((op & 0x9F000000U) != 0x10000000U)
        return 0;
    immediate = ((uint64_t)(op >> 29) & 3U) |
                (((uint64_t)(op >> 5) & 0x7FFFFU) << 2);
    displacement = sign_extend(immediate, 21);
    *target = displacement < 0 && (uint64_t)(-displacement) > pc
                  ? 0
                  : (uint64_t)((int64_t)pc + displacement);
    return 1;
}

static int decode_adrp_target(uint32_t op, uint64_t pc, uint64_t *target)
{
    uint64_t immediate;
    int64_t pages;
    int64_t displacement;
    uint64_t page = pc & ~0xFFFULL;

    if ((op & 0x9F000000U) != 0x90000000U)
        return 0;
    immediate = ((uint64_t)(op >> 29) & 3U) |
                (((uint64_t)(op >> 5) & 0x7FFFFU) << 2);
    pages = sign_extend(immediate, 21);
    displacement = pages * 4096;
    if (displacement < 0 && (uint64_t)(-displacement) > page)
        return 0;
    *target = (uint64_t)((int64_t)page + displacement);
    return 1;
}

static int decode_add_immediate(
    uint32_t op, int expected_source, uint64_t *immediate, int *destination)
{
    int source;

    if ((op & 0xFF000000U) != 0x91000000U)
        return 0;
    source = (op >> 5) & 31;
    if (source != expected_source)
        return 0;
    *destination = op & 31;
    *immediate = (op >> 10) & 0xFFFU;
    if ((op >> 22) & 1U)
        *immediate <<= 12;
    return 1;
}

static int read_pointer_va(
    const macho_t *macho, uint64_t field_va, uint64_t *resolved)
{
    uint64_t offset;
    uint64_t raw;
    uint64_t target;

    if (va_to_offset(macho, field_va, &offset) != 0 ||
        !range_ok(macho->size, offset, 8))
        return -1;
    raw = read_u64(macho->data + offset);
    if (raw == 0) {
        *resolved = 0;
        return 0;
    }
    if (valid_va(macho, raw)) {
        *resolved = raw;
        return 0;
    }
    if (valid_va(macho, raw & ~7ULL)) {
        *resolved = raw;
        return 0;
    }
    if ((raw >> 62) & 1U)
        return -1;
    if (raw >> 63)
        target = raw & 0xFFFFFFFFULL;
    else
        target = raw & ((1ULL << 43) - 1);
    if (!valid_va(macho, target))
        target += macho->image_base;
    if (!valid_va(macho, target))
        return -1;
    *resolved = target;
    return 0;
}

static const char *string_at_va(const macho_t *macho, uint64_t va)
{
    uint64_t offset;
    const uint8_t *end;

    if (va_to_offset(macho, va, &offset) != 0)
        return NULL;
    end = memchr(macho->data + offset, '\0', macho->size - (size_t)offset);
    return end ? (const char *)(macho->data + offset) : NULL;
}

static int code_references_address(
    const macho_t *macho, uint64_t instruction_va, uint64_t wanted)
{
    uint64_t offset;
    uint64_t target;
    uint32_t op;

    if (va_to_offset(macho, instruction_va, &offset) != 0 ||
        !range_ok(macho->size, offset, 4))
        return 0;
    op = read_u32(macho->data + offset);
    if (decode_adr_target(op, instruction_va, &target))
        return target == wanted;
    if (decode_adrp_target(op, instruction_va, &target)) {
        int page_register = op & 31;
        unsigned step;
        for (step = 1; step <= 2; step++) {
            uint64_t next_va = instruction_va + (uint64_t)step * 4;
            uint64_t next_offset;
            uint64_t immediate;
            int destination;
            uint32_t next;

            if (va_to_offset(macho, next_va, &next_offset) != 0 ||
                !range_ok(macho->size, next_offset, 4))
                break;
            next = read_u32(macho->data + next_offset);
            if (decode_add_immediate(next, page_register, &immediate,
                                     &destination))
                return destination == page_register &&
                       immediate <= UINT64_MAX - target &&
                       target + immediate == wanted;
            if (next != 0xD503201FU)
                break;
        }
    }
    return 0;
}

static int find_cstring_xref_function(
    const macho_t *macho, const char *wanted, uint64_t *function_start,
    uint64_t *function_end)
{
    const section_info_t *text = find_section(macho, "__TEXT", "__text");
    uint64_t selected_start = 0;
    uint64_t selected_end = 0;
    unsigned matches = 0;
    size_t section_index;

    if (!text || !macho->has_function_starts) {
        fprintf(stderr,
                "C-string XREF anchors require __TEXT,__text and LC_FUNCTION_STARTS\n");
        return -1;
    }
    for (section_index = 0; section_index < macho->section_count;
         section_index++) {
        const section_info_t *section = &macho->sections[section_index];
        uint64_t string_cursor = 0;

        if (strcmp(section->sectname, "__cstring") != 0 &&
            (section->flags & SECTION_TYPE) != S_CSTRING_LITERALS)
            continue;
        while (string_cursor < section->size) {
            const char *value = (const char *)(macho->data + section->offset +
                                               string_cursor);
            size_t remaining = (size_t)(section->size - string_cursor);
            const char *terminator = memchr(value, '\0', remaining);
            size_t length;
            uint64_t string_va;
            uint64_t code_va;

            if (!terminator)
                break;
            length = (size_t)(terminator - value);
            string_va = section->addr + string_cursor;
            string_cursor += length + 1;
            if (strcmp(value, wanted) != 0)
                continue;
            for (code_va = text->addr; code_va + 4 <= text->addr + text->size;
                 code_va += 4) {
                uint64_t start;
                uint64_t end;
                if (!code_references_address(macho, code_va, string_va) ||
                    function_range_containing(macho, code_va, &start, &end) != 0)
                    continue;
                if (!selected_start) {
                    selected_start = start;
                    selected_end = end;
                    matches = 1;
                } else if (selected_start != start) {
                    matches = 2;
                }
            }
        }
    }
    if (matches != 1) {
        fprintf(stderr, "expected one function referencing C-string %s, found %u\n",
                wanted, matches);
        return -1;
    }
    *function_start = selected_start;
    *function_end = selected_end;
    return 0;
}

static const char *method_name_at(const macho_t *macho, uint64_t name_va)
{
    size_t index;
    uint64_t indirect;

    for (index = 0; index < macho->section_count; index++) {
        const section_info_t *section = &macho->sections[index];
        if (strcmp(section->sectname, "__objc_methname") == 0 &&
            name_va >= section->addr && name_va - section->addr < section->size)
            return string_at_va(macho, name_va);
    }
    if (read_pointer_va(macho, name_va, &indirect) != 0)
        return NULL;
    return string_at_va(macho, indirect);
}

static int method_name_matches(
    const macho_t *macho, uint64_t name_va, const char *selector)
{
    const char *name = method_name_at(macho, name_va);
    return name && strcmp(name, selector) == 0;
}

static int find_method_in_list(
    const macho_t *macho, uint64_t list_va, const char *selector,
    uint64_t *implementation)
{
    uint64_t offset;
    uint32_t flags;
    uint32_t count;
    uint32_t entry_size;
    uint32_t index;
    int relative;

    if (!list_va || va_to_offset(macho, list_va, &offset) != 0 ||
        !range_ok(macho->size, offset, 8))
        return 0;
    flags = read_u32(macho->data + offset);
    count = read_u32(macho->data + offset + 4);
    relative = (flags & 0x80000000U) != 0;
    entry_size = flags & 0xFFFFU;
    entry_size &= ~3U;
    if ((!relative && entry_size < 24) || (relative && entry_size < 12) ||
        count > 100000)
        return 0;
    if (!range_ok(macho->size, offset + 8, (uint64_t)count * entry_size))
        return 0;

    for (index = 0; index < count; index++) {
        uint64_t entry_va = list_va + 8 + (uint64_t)index * entry_size;
        uint64_t entry_offset = offset + 8 + (uint64_t)index * entry_size;
        uint64_t name_va;
        uint64_t imp_va;

        if (relative) {
            name_va = entry_va + read_i32(macho->data + entry_offset);
            imp_va = entry_va + 8 + read_i32(macho->data + entry_offset + 8);
        } else {
            if (read_pointer_va(macho, entry_va, &name_va) != 0 ||
                read_pointer_va(macho, entry_va + 16, &imp_va) != 0)
                continue;
        }
        if (method_name_matches(macho, name_va, selector)) {
            *implementation = imp_va;
            return 1;
        }
    }
    return 0;
}

static int class_ro_from_class(
    const macho_t *macho, uint64_t class_va, uint64_t *ro_va)
{
    uint64_t data_va;
    if (read_pointer_va(macho, class_va + 32, &data_va) != 0)
        return -1;
    data_va &= ~7ULL;
    if (!valid_va(macho, data_va))
        return -1;
    *ro_va = data_va;
    return 0;
}

static int find_objc_method(
    const macho_t *macho, const char *class_name, const char *selector,
    int class_method, uint64_t *implementation)
{
    const section_info_t *classlist =
        find_section(macho, "__DATA_CONST", "__objc_classlist");
    uint64_t cursor;
    unsigned matches = 0;

    if (!classlist)
        classlist = find_section(macho, NULL, "__objc_classlist");
    if (!classlist || classlist->size % 8 != 0) {
        fprintf(stderr, "Objective-C class list was not found\n");
        return -1;
    }
    for (cursor = 0; cursor < classlist->size; cursor += 8) {
        uint64_t slot_va = classlist->addr + cursor;
        uint64_t class_va;
        uint64_t ro_va;
        uint64_t name_va;
        uint64_t methods_va;
        const char *name;

        if (read_pointer_va(macho, slot_va, &class_va) != 0)
            continue;
        if (class_method && read_pointer_va(macho, class_va, &class_va) != 0)
            continue;
        if (class_ro_from_class(macho, class_va, &ro_va) != 0 ||
            read_pointer_va(macho, ro_va + 24, &name_va) != 0 ||
            read_pointer_va(macho, ro_va + 32, &methods_va) != 0)
            continue;
        name = string_at_va(macho, name_va);
        if (!name || strcmp(name, class_name) != 0)
            continue;
        if (find_method_in_list(macho, methods_va, selector, implementation))
            matches++;
    }
    if (matches != 1) {
        fprintf(stderr,
                "expected one Objective-C method %c[%s %s], found %u\n",
                class_method ? '+' : '-', class_name, selector, matches);
        return -1;
    }
    return 0;
}

static int find_symbol(
    const macho_t *macho, const char *wanted, uint64_t *address)
{
    const symtab_command_t *symtab = &macho->symtab;
    const nlist_64_t *symbols;
    const char *strings;
    uint32_t index;
    unsigned matches = 0;

    if (!macho->has_symtab ||
        !range_ok(macho->size, symtab->symoff,
                  (uint64_t)symtab->nsyms * sizeof(nlist_64_t)) ||
        !range_ok(macho->size, symtab->stroff, symtab->strsize)) {
        fprintf(stderr, "Mach-O symbol table is unavailable\n");
        return -1;
    }
    symbols = (const nlist_64_t *)(macho->data + symtab->symoff);
    strings = (const char *)(macho->data + symtab->stroff);
    for (index = 0; index < symtab->nsyms; index++) {
        const nlist_64_t *symbol = &symbols[index];
        const char *name;
        if (!symbol->n_value || symbol->n_strx >= symtab->strsize)
            continue;
        name = strings + symbol->n_strx;
        if (memchr(name, '\0', symtab->strsize - symbol->n_strx) &&
            strcmp(name, wanted) == 0) {
            *address = symbol->n_value;
            matches++;
        }
    }
    if (matches != 1) {
        fprintf(stderr, "expected one function symbol %s, found %u\n",
                wanted, matches);
        return -1;
    }
    return 0;
}

static int address_is_in_function(
    const macho_t *macho, uint64_t implementation, uint64_t target)
{
    uint64_t va;

    if (target < implementation || target - implementation >= 0x1000 ||
        ((target - implementation) & 3) != 0)
        return 0;
    for (va = implementation; va < target; va += 4) {
        uint64_t offset;
        uint32_t op;
        if (va_to_offset(macho, va, &offset) != 0 ||
            !range_ok(macho->size, offset, 4))
            return 0;
        op = read_u32(macho->data + offset);
        if ((op & 0xFFFFFC1FU) == 0xD65F0000U || op == 0xD65F0FFFU)
            return 0;
    }
    return 1;
}

static void copy_string(char *output, size_t output_size, const char *input)
{
    if (!output_size)
        return;
    snprintf(output, output_size, "%s", input ? input : "");
}

static void consider_method_list_owner(
    const macho_t *macho, uint64_t list_va, const char *class_name,
    int class_method, uint64_t target, owner_t *owner)
{
    uint64_t offset;
    uint32_t flags;
    uint32_t count;
    uint32_t entry_size;
    uint32_t index;
    int relative;

    if (!list_va || va_to_offset(macho, list_va, &offset) != 0 ||
        !range_ok(macho->size, offset, 8))
        return;
    flags = read_u32(macho->data + offset);
    count = read_u32(macho->data + offset + 4);
    relative = (flags & 0x80000000U) != 0;
    entry_size = (flags & 0xFFFFU) & ~3U;
    if ((!relative && entry_size < 24) || (relative && entry_size < 12) ||
        count > 100000 ||
        !range_ok(macho->size, offset + 8, (uint64_t)count * entry_size))
        return;

    for (index = 0; index < count; index++) {
        uint64_t entry_va = list_va + 8 + (uint64_t)index * entry_size;
        uint64_t entry_offset = offset + 8 + (uint64_t)index * entry_size;
        uint64_t name_va;
        uint64_t imp_va;
        const char *selector;

        if (relative) {
            name_va = entry_va + read_i32(macho->data + entry_offset);
            imp_va = entry_va + 8 + read_i32(macho->data + entry_offset + 8);
        } else {
            if (read_pointer_va(macho, entry_va, &name_va) != 0 ||
                read_pointer_va(macho, entry_va + 16, &imp_va) != 0)
                continue;
        }
        selector = method_name_at(macho, name_va);
        if (!selector || !address_is_in_function(macho, imp_va, target))
            continue;
        if (!owner->found || imp_va > owner->implementation) {
            owner->found = 1;
            owner->is_objc = 1;
            owner->class_method = class_method;
            owner->implementation = imp_va;
            copy_string(owner->class_name, sizeof(owner->class_name), class_name);
            copy_string(owner->selector, sizeof(owner->selector), selector);
        }
    }
}

static void consider_class_owner(
    const macho_t *macho, uint64_t class_va, int class_method,
    uint64_t target, owner_t *owner)
{
    uint64_t ro_va;
    uint64_t name_va;
    uint64_t methods_va;
    const char *class_name;

    if (class_method && read_pointer_va(macho, class_va, &class_va) != 0)
        return;
    if (class_ro_from_class(macho, class_va, &ro_va) != 0 ||
        read_pointer_va(macho, ro_va + 24, &name_va) != 0 ||
        read_pointer_va(macho, ro_va + 32, &methods_va) != 0)
        return;
    class_name = string_at_va(macho, name_va);
    if (!class_name)
        return;
    consider_method_list_owner(
        macho, methods_va, class_name, class_method, target, owner);
}

static void find_objc_owner(
    const macho_t *macho, uint64_t target, owner_t *owner)
{
    const section_info_t *classlist =
        find_section(macho, "__DATA_CONST", "__objc_classlist");
    uint64_t cursor;

    if (!classlist)
        classlist = find_section(macho, NULL, "__objc_classlist");
    if (!classlist || classlist->size % 8 != 0)
        return;
    for (cursor = 0; cursor < classlist->size; cursor += 8) {
        uint64_t class_va;
        if (read_pointer_va(macho, classlist->addr + cursor, &class_va) != 0)
            continue;
        consider_class_owner(macho, class_va, 0, target, owner);
        consider_class_owner(macho, class_va, 1, target, owner);
    }
}

static void find_symbol_owner(
    const macho_t *macho, uint64_t target, owner_t *owner)
{
    const symtab_command_t *symtab = &macho->symtab;
    const nlist_64_t *symbols;
    const char *strings;
    uint32_t index;

    if (owner->found || !macho->has_symtab ||
        !range_ok(macho->size, symtab->symoff,
                  (uint64_t)symtab->nsyms * sizeof(nlist_64_t)) ||
        !range_ok(macho->size, symtab->stroff, symtab->strsize))
        return;
    symbols = (const nlist_64_t *)(macho->data + symtab->symoff);
    strings = (const char *)(macho->data + symtab->stroff);
    for (index = 0; index < symtab->nsyms; index++) {
        const nlist_64_t *symbol = &symbols[index];
        const char *name;
        if (!symbol->n_value || symbol->n_strx >= symtab->strsize ||
            !address_is_in_function(macho, symbol->n_value, target))
            continue;
        name = strings + symbol->n_strx;
        if (!memchr(name, '\0', symtab->strsize - symbol->n_strx))
            continue;
        if (!owner->found || symbol->n_value > owner->implementation) {
            owner->found = 1;
            owner->is_objc = 0;
            owner->implementation = symbol->n_value;
            copy_string(owner->function, sizeof(owner->function), name);
        }
    }
}

static void find_owner(const macho_t *macho, uint64_t target, owner_t *owner)
{
    memset(owner, 0, sizeof(*owner));
    find_objc_owner(macho, target, owner);
    find_symbol_owner(macho, target, owner);
}

static int parse_register(const char *text, char expected_class, int *value)
{
    char *end;
    long number;

    if (!text)
        return 0;
    if (text[0] != expected_class && text[0] != expected_class + ('a' - 'A'))
        return -1;
    if (text[1] == '?' && text[2] == '\0') {
        *value = -2;
        return 1;
    }
    errno = 0;
    number = strtol(text + 1, &end, 10);
    if (errno || *end || number < 0 || number > 31)
        return -1;
    *value = (int)number;
    return 1;
}

static int parse_number_or_wildcard(
    const char *text, int *present, uint64_t *value)
{
    char *end;
    unsigned long long parsed;

    if (!text)
        return 0;
    if (strcmp(text, "*") == 0) {
        *present = 0;
        return 1;
    }
    errno = 0;
    parsed = strtoull(text, &end, 0);
    if (errno || *end)
        return -1;
    *present = 1;
    *value = parsed;
    return 1;
}

static int decode_instruction(uint32_t op, decoded_t *decoded)
{
    memset(decoded, 0, sizeof(*decoded));
    decoded->destination = -1;
    decoded->source = -1;
    decoded->base = -1;
    decoded->condition = -1;

    if ((op & 0xFFC00000U) == 0x39400000U) {
        decoded->mnemonic = "LDRB";
        decoded->destination = op & 31;
        decoded->destination_class = 'W';
        decoded->base = (op >> 5) & 31;
        decoded->has_offset = 1;
        decoded->offset = (op >> 10) & 0xFFF;
    } else if ((op & 0xBFC00000U) == 0xB9400000U) {
        decoded->mnemonic = "LDR";
        decoded->destination = op & 31;
        decoded->destination_class = (op >> 31) ? 'X' : 'W';
        decoded->base = (op >> 5) & 31;
        decoded->has_offset = 1;
        decoded->offset = ((op >> 10) & 0xFFF) << ((op >> 30) & 3);
    } else if ((op & 0xBFC00000U) == 0xB9000000U) {
        decoded->mnemonic = "STR";
        decoded->source = op & 31;
        decoded->source_class = (op >> 31) ? 'X' : 'W';
        decoded->base = (op >> 5) & 31;
        decoded->has_offset = 1;
        decoded->offset = ((op >> 10) & 0xFFF) << ((op >> 30) & 3);
    } else if ((op & 0x7F800000U) == 0x52800000U) {
        decoded->mnemonic = "MOV";
        decoded->destination = op & 31;
        decoded->destination_class = (op >> 31) ? 'X' : 'W';
        decoded->has_immediate = 1;
        decoded->immediate = ((op >> 5) & 0xFFFF) << (((op >> 21) & 3) * 16);
    } else if ((op & 0x7FE0FFE0U) == 0x2A0003E0U) {
        decoded->mnemonic = "MOV";
        decoded->destination = op & 31;
        decoded->destination_class = (op >> 31) ? 'X' : 'W';
        decoded->source = (op >> 16) & 31;
        decoded->source_class = decoded->destination_class;
    } else if ((op & 0x7F00001FU) == 0x7100001FU) {
        decoded->mnemonic = "CMP";
        decoded->source = (op >> 5) & 31;
        decoded->source_class = (op >> 31) ? 'X' : 'W';
        decoded->has_immediate = 1;
        decoded->immediate = (op >> 10) & 0xFFF;
        if ((op >> 22) & 1)
            decoded->immediate <<= 12;
    } else if ((op & 0x7FFF0FE0U) == 0x1A9F07E0U) {
        unsigned encoded_condition = (op >> 12) & 0xFU;
        if (encoded_condition >= 14)
            return 0;
        decoded->mnemonic = "CSET";
        decoded->destination = op & 31;
        decoded->destination_class = (op >> 31) ? 'X' : 'W';
        decoded->condition = (int)(encoded_condition ^ 1U);
    } else if ((op & 0x7F000000U) == 0x34000000U) {
        decoded->mnemonic = (op & 0x01000000U) ? "CBNZ" : "CBZ";
        decoded->source = op & 31;
        decoded->source_class = (op >> 31) ? 'X' : 'W';
    } else if ((op & 0x7F000000U) == 0x36000000U) {
        decoded->mnemonic = (op & 0x01000000U) ? "TBNZ" : "TBZ";
        decoded->source = op & 31;
        decoded->has_immediate = 1;
        decoded->immediate = ((op >> 19) & 0x1F) | ((uint64_t)(op >> 26) & 0x20);
        decoded->source_class = decoded->immediate >= 32 ? 'X' : 'W';
    } else if ((op & 0xFC000000U) == 0x94000000U) {
        decoded->mnemonic = "BL";
    } else if ((op & 0xFC000000U) == 0x14000000U) {
        decoded->mnemonic = "B";
    } else if ((op & 0xFFFFFC1FU) == 0xD65F0000U || op == 0xD65F0FFFU) {
        decoded->mnemonic = "RET";
    } else {
        return 0;
    }
    return 1;
}

static const char *condition_name(int condition)
{
    static const char *names[] = {
        "EQ", "NE", "CS", "CC", "MI", "PL", "VS", "VC",
        "HI", "LS", "GE", "LT", "GT", "LE", "AL", "NV"
    };
    return condition >= 0 && condition < 16 ? names[condition] : NULL;
}

static int ascii_equal_ignore_case(const char *left, const char *right)
{
    while (*left && *right) {
        unsigned char a = (unsigned char)*left++;
        unsigned char b = (unsigned char)*right++;
        if (a >= 'a' && a <= 'z')
            a = (unsigned char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z')
            b = (unsigned char)(b - 'a' + 'A');
        if (a != b)
            return 0;
    }
    return !*left && !*right;
}

static int parse_condition(const char *text, int *condition)
{
    int index;
    if (!text)
        return 0;
    if (strcmp(text, "*") == 0) {
        *condition = -2;
        return 1;
    }
    for (index = 0; index < 14; index++) {
        if (ascii_equal_ignore_case(text, condition_name(index))) {
            *condition = index;
            return 1;
        }
    }
    if (ascii_equal_ignore_case(text, "HS")) {
        *condition = 2;
        return 1;
    }
    if (ascii_equal_ignore_case(text, "LO")) {
        *condition = 3;
        return 1;
    }
    return -1;
}

static int hex_nibble(char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

static int parse_expected_instruction(const char *text, uint8_t output[4])
{
    size_t count = 0;
    int high = -1;

    while (*text) {
        int nibble;
        if (*text == ' ' || *text == '\t' || *text == ':' || *text == '-') {
            text++;
            continue;
        }
        nibble = hex_nibble(*text++);
        if (nibble < 0)
            return -1;
        if (high < 0) {
            high = nibble;
        } else {
            if (count >= 4)
                return -1;
            output[count++] = (uint8_t)((high << 4) | nibble);
            high = -1;
        }
    }
    return count == 4 && high < 0 ? 0 : -1;
}

static void print_json_string(const char *value)
{
    const unsigned char *cursor = (const unsigned char *)value;
    fputc('"', json_stream);
    while (*cursor) {
        if (*cursor == '"' || *cursor == '\\')
            fputc('\\', json_stream);
        if (*cursor >= 0x20)
            fputc(*cursor, json_stream);
        else
            fprintf(json_stream, "\\u%04x", *cursor);
        cursor++;
    }
    fputc('"', json_stream);
}

static void print_register(char register_class, int number)
{
    fprintf(json_stream, "\"%c%d\"", register_class, number);
}

static void print_decoded_instruction(const decoded_t *decoded, int generalized)
{
    fprintf(json_stream, "{\"mnemonic\":");
    print_json_string(decoded->mnemonic);
    if (decoded->destination >= 0) {
        fprintf(json_stream, ",\"destination\":");
        print_register(decoded->destination_class, decoded->destination);
    }
    if (decoded->source >= 0) {
        fprintf(json_stream, ",\"source\":");
        print_register(decoded->source_class, decoded->source);
    }
    if (decoded->condition >= 0) {
        fprintf(json_stream, ",\"condition\":");
        print_json_string(condition_name(decoded->condition));
    }
    if (decoded->base >= 0) {
        fprintf(json_stream, ",\"memory\":{\"base\":");
        print_register('X', decoded->base);
        if (decoded->has_offset) {
            if (generalized)
                fprintf(json_stream, ",\"offset\":\"*\"");
            else
                fprintf(json_stream, ",\"offset\":\"0x%" PRIx64 "\"", decoded->offset);
        }
        fputc('}', json_stream);
    } else if (decoded->has_immediate) {
        fprintf(json_stream, ",\"immediate\":\"0x%" PRIx64 "\"", decoded->immediate);
    }
    fputc('}', json_stream);
}

static int generate_report(
    const macho_t *macho, const options_t *options)
{
    uint64_t file_offset;
    uint64_t va;
    uint8_t expected[4];
    const uint8_t *actual;
    uint32_t op;
    decoded_t decoded;
    owner_t owner;

    if (parse_number_or_wildcard(options->offset, &(int){0}, &file_offset) <= 0 ||
        strcmp(options->offset, "*") == 0) {
        fprintf(stderr, "--gen requires a concrete file offset\n");
        return 2;
    }
    if (parse_expected_instruction(options->expected, expected) != 0) {
        fprintf(stderr, "--expected must contain exactly four hexadecimal bytes\n");
        return 2;
    }
    if (!range_ok(macho->size, file_offset, 4) ||
        offset_to_va(macho, file_offset, &va) != 0) {
        fprintf(stderr, "file offset is outside a mapped Mach-O section\n");
        return 1;
    }
    actual = macho->data + file_offset;
    if (memcmp(expected, actual, 4) != 0) {
        fprintf(json_stream, "{\"status\":\"expected_mismatch\",\"file_offset\":"
               "\"0x%" PRIx64 "\",\"expected\":"
               "\"%02x %02x %02x %02x\",\"actual\":"
               "\"%02x %02x %02x %02x\"}\n",
               file_offset, expected[0], expected[1], expected[2], expected[3],
               actual[0], actual[1], actual[2], actual[3]);
        return 1;
    }
    op = read_u32(actual);
    if (!decode_instruction(op, &decoded)) {
        fprintf(stderr, "instruction at the supplied offset is unsupported\n");
        return 1;
    }
    find_owner(macho, va, &owner);

    fprintf(json_stream, "{\"status\":\"analyzed\",\"file_offset\":\"0x%" PRIx64
           "\",\"virtual_address\":\"0x%" PRIx64
           "\",\"expected\":\"%02x %02x %02x %02x\","
           "\"expected_matches\":true,\"owner\":",
           file_offset, va, expected[0], expected[1], expected[2], expected[3]);
    if (!owner.found) {
        fprintf(json_stream, "{\"kind\":\"unknown\"}");
    } else if (owner.is_objc) {
        fprintf(json_stream, "{\"kind\":\"objc_method\",\"class\":");
        print_json_string(owner.class_name);
        fprintf(json_stream, ",\"selector\":");
        print_json_string(owner.selector);
        fprintf(json_stream, ",\"method_kind\":\"%s\",\"implementation\":"
               "\"0x%" PRIx64 "\",\"instruction_offset\":"
               "\"0x%" PRIx64 "\"}",
               owner.class_method ? "class" : "instance",
               owner.implementation, va - owner.implementation);
    } else {
        fprintf(json_stream, "{\"kind\":\"function\",\"name\":");
        print_json_string(owner.function);
        fprintf(json_stream, ",\"implementation\":\"0x%" PRIx64
               "\",\"instruction_offset\":\"0x%" PRIx64 "\"}",
               owner.implementation, va - owner.implementation);
    }
    fprintf(json_stream, ",\"decoded_instruction\":");
    print_decoded_instruction(&decoded, 0);
    fprintf(json_stream, ",\"generalized_instruction\":");
    print_decoded_instruction(&decoded, 1);
    fprintf(json_stream, "}\n");
    return 0;
}

static int register_matches(int expected, int actual)
{
    return expected == -1 || expected == -2 || expected == actual;
}

static int instruction_matches(
    const options_t *options, uint32_t op, char *error, size_t error_size)
{
    decoded_t decoded;
    int destination = -1;
    char destination_class = 0;
    int source = -1;
    char source_class = 0;
    int base = -1;
    int has_offset = 0;
    int has_immediate = 0;
    int condition = -1;
    uint64_t offset = 0;
    uint64_t immediate = 0;

    if (!decode_instruction(op, &decoded) ||
        strcmp(decoded.mnemonic, options->mnemonic) != 0)
        return 0;
    if (options->destination) {
        destination_class = (char)(options->destination[0] & ~0x20);
        if ((destination_class != 'W' && destination_class != 'X') ||
            parse_register(options->destination, destination_class,
                           &destination) < 0) {
            snprintf(error, error_size, "invalid destination register");
            return -1;
        }
    }
    if (options->source) {
        source_class = (char)(options->source[0] & ~0x20);
        if ((source_class != 'W' && source_class != 'X') ||
            parse_register(options->source, source_class, &source) < 0) {
            snprintf(error, error_size, "invalid source register");
            return -1;
        }
    }
    if (options->base && parse_register(options->base, 'X', &base) < 0) {
        snprintf(error, error_size, "invalid memory base register");
        return -1;
    }
    if (parse_number_or_wildcard(options->offset, &has_offset, &offset) < 0 ||
        parse_number_or_wildcard(options->immediate, &has_immediate,
                                 &immediate) < 0) {
        snprintf(error, error_size, "invalid immediate or memory offset");
        return -1;
    }
    if (parse_condition(options->condition, &condition) < 0) {
        snprintf(error, error_size, "invalid ARM64 condition");
        return -1;
    }
    if (!register_matches(destination, decoded.destination) ||
        !register_matches(source, decoded.source) ||
        !register_matches(base, decoded.base))
        return 0;
    if ((destination_class &&
         destination_class != decoded.destination_class) ||
        (source_class && source_class != decoded.source_class))
        return 0;
    if (options->offset && !decoded.has_offset)
        return 0;
    if (has_offset && decoded.offset != offset)
        return 0;
    if (options->immediate && !decoded.has_immediate)
        return 0;
    if (has_immediate && decoded.immediate != immediate)
        return 0;
    if (options->condition && decoded.condition < 0)
        return 0;
    if (condition >= 0 && decoded.condition != condition)
        return 0;
    return 1;
}

static int find_instruction(
    const macho_t *macho, const options_t *options, uint64_t function_va,
    uint64_t function_end, uint64_t *match_va, uint32_t *match_op)
{
    const section_info_t *text = find_section(macho, "__TEXT", "__text");
    uint64_t start_offset;
    uint64_t end_va;
    uint64_t va;
    unsigned matches = 0;
    char error[128] = {0};

    if (!text || function_va < text->addr || function_va >= text->addr + text->size ||
        va_to_offset(macho, function_va, &start_offset) != 0) {
        fprintf(stderr, "function implementation is outside __TEXT,__text\n");
        return -1;
    }
    end_va = function_end > function_va ? function_end : function_va + 0x1000;
    if (end_va > text->addr + text->size)
        end_va = text->addr + text->size;
    for (va = function_va; va + 4 <= end_va; va += 4) {
        uint64_t offset;
        uint32_t op;
        int result;

        if (va_to_offset(macho, va, &offset) != 0 ||
            !range_ok(macho->size, offset, 4))
            break;
        op = read_u32(macho->data + offset);
        result = instruction_matches(options, op, error, sizeof(error));
        if (result < 0) {
            fprintf(stderr, "%s\n", error);
            return -1;
        }
        if (result > 0) {
            *match_va = va;
            *match_op = op;
            matches++;
        }
        if (!function_end &&
            ((op & 0xFFFFFC1FU) == 0xD65F0000U || op == 0xD65F0FFFU))
            break;
    }
    if (matches != 1) {
        fprintf(stderr, "expected one matching instruction, found %u\n", matches);
        return -1;
    }
    (void)start_offset;
    return 0;
}

static void usage(FILE *stream)
{
    fprintf(stream,
            "usage:\n"
            "  custombin_patchfinder --gen --input FILE --offset FILE_OFFSET "
            "--expected HEX_BYTES [--output FILE]\n"
            "  custombin_patchfinder --input FILE "
            "(--function NAME | --objc-class NAME --selector NAME | "
            "--cstring-xref VALUE) "
            "--mnemonic NAME [matcher options] [--output FILE]\n"
            "matcher options: --destination W0|W?|X0|X? --source REG "
            "--base X0|X? --offset VALUE|* --immediate VALUE|* "
            "--condition EQ|NE|...|* --method-kind instance|class\n");
}

static int parse_options(int argc, char **argv, options_t *options)
{
    int index;
    memset(options, 0, sizeof(*options));
    options->method_kind = "instance";
    for (index = 1; index < argc; index++) {
        const char **field = NULL;
        if (strcmp(argv[index], "--gen") == 0) {
            options->generate = 1;
            continue;
        } else if (strcmp(argv[index], "--output") == 0) {
            field = &options->output;
        } else if (strcmp(argv[index], "--input") == 0)
            field = &options->input;
        else if (strcmp(argv[index], "--function") == 0)
            field = &options->function;
        else if (strcmp(argv[index], "--cstring-xref") == 0)
            field = &options->cstring_xref;
        else if (strcmp(argv[index], "--objc-class") == 0)
            field = &options->objc_class;
        else if (strcmp(argv[index], "--selector") == 0)
            field = &options->selector;
        else if (strcmp(argv[index], "--method-kind") == 0)
            field = &options->method_kind;
        else if (strcmp(argv[index], "--mnemonic") == 0)
            field = &options->mnemonic;
        else if (strcmp(argv[index], "--destination") == 0)
            field = &options->destination;
        else if (strcmp(argv[index], "--source") == 0)
            field = &options->source;
        else if (strcmp(argv[index], "--base") == 0)
            field = &options->base;
        else if (strcmp(argv[index], "--offset") == 0)
            field = &options->offset;
        else if (strcmp(argv[index], "--immediate") == 0)
            field = &options->immediate;
        else if (strcmp(argv[index], "--condition") == 0)
            field = &options->condition;
        else if (strcmp(argv[index], "--expected") == 0)
            field = &options->expected;
        else {
            fprintf(stderr, "unknown option: %s\n", argv[index]);
            return -1;
        }
        if (++index >= argc) {
            fprintf(stderr, "missing value for %s\n", argv[index - 1]);
            return -1;
        }
        *field = argv[index];
    }
    if (!options->input ||
        (options->generate && (!options->offset || !options->expected)) ||
        (!options->generate &&
         (!options->mnemonic ||
          ((!!options->function + !!options->objc_class +
            !!options->cstring_xref) != 1) ||
          (options->objc_class && !options->selector))) ||
        (strcmp(options->method_kind, "instance") != 0 &&
         strcmp(options->method_kind, "class") != 0)) {
        usage(stderr);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    options_t options;
    macho_t macho;
    uint64_t function_va;
    uint64_t function_end = 0;
    uint64_t match_va;
    uint64_t match_offset;
    uint32_t match_op;
    const uint8_t *bytes;
    FILE *output_file = NULL;
    int status;

    if (parse_options(argc, argv, &options) != 0)
        return 2;
    json_stream = stdout;
    if (options.output) {
        output_file = fopen(options.output, "w");
        if (!output_file) {
            fprintf(stderr, "cannot open output %s: %s\n", options.output,
                    strerror(errno));
            return 1;
        }
        json_stream = output_file;
    }
    if (parse_macho(&macho, options.input) != 0) {
        if (output_file)
            fclose(output_file);
        free(macho.data);
        return 1;
    }
    if (options.generate) {
        status = generate_report(&macho, &options);
        if (output_file)
            fclose(output_file);
        free(macho.data);
        return status;
    }
    if (options.function) {
        if (find_symbol(&macho, options.function, &function_va) != 0) {
            if (output_file)
                fclose(output_file);
            free(macho.data);
            return 1;
        }
    } else if (options.objc_class && find_objc_method(
                   &macho, options.objc_class, options.selector,
                   strcmp(options.method_kind, "class") == 0,
                   &function_va) != 0) {
        if (output_file)
            fclose(output_file);
        free(macho.data);
        return 1;
    } else if (options.cstring_xref &&
               find_cstring_xref_function(
                   &macho, options.cstring_xref, &function_va,
                   &function_end) != 0) {
        if (output_file)
            fclose(output_file);
        free(macho.data);
        return 1;
    }
    if (!function_end) {
        uint64_t detected_start;
        uint64_t detected_end;
        if (function_range_containing(
                &macho, function_va, &detected_start, &detected_end) == 0 &&
            detected_start == function_va)
            function_end = detected_end;
    }
    if (find_instruction(&macho, &options, function_va, function_end, &match_va,
                         &match_op) != 0 ||
        va_to_offset(&macho, match_va, &match_offset) != 0) {
        if (output_file)
            fclose(output_file);
        free(macho.data);
        return 1;
    }
    bytes = (const uint8_t *)&match_op;
    fprintf(json_stream, "{\"status\":\"found\",\"file_offset\":\"0x%" PRIx64
           "\",\"virtual_address\":\"0x%" PRIx64
           "\",\"size\":4,\"bytes\":\"%02x %02x %02x %02x\","
           "\"matches\":1}\n",
           match_offset, match_va, bytes[0], bytes[1], bytes[2], bytes[3]);
    if (output_file)
        fclose(output_file);
    free(macho.data);
    return 0;
}
