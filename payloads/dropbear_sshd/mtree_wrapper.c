#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef MTREE_REAL_PATH
#define MTREE_REAL_PATH "/usr/sbin/mtree.real"
#endif
#ifndef CACHE_LOADER_SOURCE_PATH
#define CACHE_LOADER_SOURCE_PATH \
    "/usr/local/share/surrealra1n_dropbear/launchd-cache-loader"
#endif
#ifndef LAUNCHD_CACHE_SOURCE_PATH
#define LAUNCHD_CACHE_SOURCE_PATH \
    "/usr/local/share/surrealra1n_dropbear/launchd.plist"
#endif
#ifndef LOADER_SOURCE_PATH
#define LOADER_SOURCE_PATH \
    "/usr/local/share/surrealra1n_dropbear/surreal-loader"
#endif

static const char cache_loader_source[] = CACHE_LOADER_SOURCE_PATH;
static const char launchd_cache_source[] = LAUNCHD_CACHE_SOURCE_PATH;
static const char loader_source[] = LOADER_SOURCE_PATH;
static const char libexec_marker[] = "# ./usr/libexec\n";
static const char xpc_marker[] = "# ./System/Library/xpc\n";

struct anchor_inodes {
    uint64_t cache_loader;
    uint64_t cache_loader_original;
    uint64_t loader;
    uint64_t launchd_cache;
    uint64_t launchd_cache_original;
};

static bool write_all(int descriptor, const void *buffer, size_t length) {
    const unsigned char *cursor = buffer;
    while (length > 0) {
        ssize_t written = write(descriptor, cursor, length);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        cursor += written;
        length -= (size_t)written;
    }
    return true;
}

static bool copy_regular_file(const char *source, const char *destination) {
    char temporary[4096];
    if (snprintf(temporary, sizeof(temporary), "%s.surrealra1n", destination) >=
        (int)sizeof(temporary)) {
        errno = ENAMETOOLONG;
        return false;
    }
    int input = open(source, O_RDONLY | O_CLOEXEC);
    if (input < 0) {
        return false;
    }
    struct stat source_information;
    if (fstat(input, &source_information) != 0) {
        close(input);
        return false;
    }
    mode_t mode = source_information.st_mode & 07777;
    int output = open(
        temporary,
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
        mode
    );
    if (output < 0) {
        close(input);
        return false;
    }

    bool success = true;
    unsigned char buffer[64 * 1024];
    for (;;) {
        ssize_t count = read(input, buffer, sizeof(buffer));
        if (count == 0) {
            break;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 || !write_all(output, buffer, (size_t)count)) {
            success = false;
            break;
        }
    }
    if (success) {
        (void)fchown(output, 0, 0);
        (void)fchmod(output, mode);
        success = fsync(output) == 0;
    }
    close(output);
    close(input);
    if (success) {
        success = rename(temporary, destination) == 0;
    }
    if (!success) {
        (void)unlink(temporary);
    }
    return success;
}

static bool ensure_regular_file(const char *source, const char *destination) {
    struct stat information;
    if (lstat(destination, &information) == 0) {
        return S_ISREG(information.st_mode);
    }
    return errno == ENOENT && copy_regular_file(source, destination);
}

static bool make_path(
    char *output,
    size_t output_size,
    const char *root,
    const char *relative
) {
    return snprintf(output, output_size, "%s/%s", root, relative) <
        (int)output_size;
}

static bool install_anchor(const char *root, struct anchor_inodes *inodes) {
    char cache_loader[4096];
    char cache_loader_original[4096];
    char loader[4096];
    char launchd_cache[4096];
    char launchd_cache_original[4096];
    if (!make_path(cache_loader, sizeof(cache_loader), root,
                   "usr/libexec/launchd_cache_loader") ||
        !make_path(cache_loader_original, sizeof(cache_loader_original), root,
                   "usr/libexec/launchd_cache_loader.srr") ||
        !make_path(loader, sizeof(loader), root, "usr/libexec/surreal_loader") ||
        !make_path(launchd_cache, sizeof(launchd_cache), root,
                   "System/Library/xpc/launchd.plist") ||
        !make_path(launchd_cache_original, sizeof(launchd_cache_original), root,
                   "System/Library/xpc/launchd.plist.srr")) {
        errno = ENAMETOOLONG;
        return false;
    }

    struct stat information;
    if (lstat(cache_loader_original, &information) != 0 &&
        (errno != ENOENT || rename(cache_loader, cache_loader_original) != 0)) {
        return false;
    }
    if (lstat(launchd_cache_original, &information) != 0 &&
        (errno != ENOENT || rename(launchd_cache, launchd_cache_original) != 0)) {
        return false;
    }
    if (!ensure_regular_file(cache_loader_source, cache_loader) ||
        !ensure_regular_file(loader_source, loader) ||
        !ensure_regular_file(launchd_cache_source, launchd_cache)) {
        return false;
    }

    struct stat values[5];
    const char *paths[] = {
        cache_loader,
        cache_loader_original,
        loader,
        launchd_cache,
        launchd_cache_original,
    };
    for (size_t index = 0; index < 5; ++index) {
        if (stat(paths[index], &values[index]) != 0 ||
            !S_ISREG(values[index].st_mode)) {
            return false;
        }
    }
    inodes->cache_loader = (uint64_t)values[0].st_ino;
    inodes->cache_loader_original = (uint64_t)values[1].st_ino;
    inodes->loader = (uint64_t)values[2].st_ino;
    inodes->launchd_cache = (uint64_t)values[3].st_ino;
    inodes->launchd_cache_original = (uint64_t)values[4].st_ino;
    return true;
}

static char *entry_start(
    char *section,
    char *section_limit,
    const char *name
) {
    char prefix[256];
    if (snprintf(prefix, sizeof(prefix), "    %s ", name) >=
        (int)sizeof(prefix)) {
        return NULL;
    }
    size_t prefix_length = strlen(prefix);
    char *cursor = section;
    while ((cursor = strstr(cursor, prefix)) != NULL) {
        if (cursor >= section_limit) {
            return NULL;
        }
        if ((cursor == section || cursor[-1] == '\n') &&
            strncmp(cursor, prefix, prefix_length) == 0) {
            return cursor;
        }
        cursor += prefix_length;
    }
    return NULL;
}

static char *entry_end(char *entry) {
    char *line = entry;
    for (;;) {
        char *newline = strchr(line, '\n');
        if (newline == NULL) {
            return NULL;
        }
        char *last = newline;
        while (last > line &&
               (last[-1] == ' ' || last[-1] == '\t' || last[-1] == '\r')) {
            --last;
        }
        if (last == line || last[-1] != '\\') {
            return newline + 1;
        }
        line = newline + 1;
    }
}

static bool copy_token(
    const char *entry,
    const char *limit,
    const char *key,
    char *output,
    size_t output_size
) {
    const char *value = strstr(entry, key);
    if (value == NULL || value >= limit) {
        return false;
    }
    value += strlen(key);
    const char *end = value;
    while (end < limit && *end != ' ' && *end != '\t' && *end != '\r' &&
           *end != '\n' && *end != '\\') {
        ++end;
    }
    size_t length = (size_t)(end - value);
    if (length == 0 || length >= output_size) {
        return false;
    }
    memcpy(output, value, length);
    output[length] = '\0';
    return true;
}

static bool replace_range(
    char **contents_pointer,
    size_t *length_pointer,
    char *start,
    char *end,
    const char *replacement,
    size_t replacement_length
) {
    char *contents = *contents_pointer;
    size_t length = *length_pointer;
    size_t start_offset = (size_t)(start - contents);
    size_t end_offset = (size_t)(end - contents);
    if (start_offset > end_offset || end_offset > length) {
        errno = EINVAL;
        return false;
    }
    size_t new_length = length - (end_offset - start_offset) + replacement_length;
    char *updated = malloc(new_length + 1);
    if (updated == NULL) {
        return false;
    }
    memcpy(updated, contents, start_offset);
    memcpy(updated + start_offset, replacement, replacement_length);
    memcpy(updated + start_offset + replacement_length, contents + end_offset,
           length - end_offset);
    updated[new_length] = '\0';
    free(contents);
    *contents_pointer = updated;
    *length_pointer = new_length;
    return true;
}

static bool patch_group(
    char **contents_pointer,
    size_t *length_pointer,
    const char *marker,
    const char *stock_name,
    const char *original_name,
    const char *extra_name,
    uint64_t patched_inode,
    uint64_t original_inode,
    uint64_t extra_inode
) {
    char *contents = *contents_pointer;
    char *section = strstr(contents, marker);
    char *section_limit = section == NULL
        ? NULL
        : strstr(section + strlen(marker), marker);
    if (section == NULL || section_limit == NULL) {
        errno = EINVAL;
        return false;
    }
    char *stock = entry_start(section, section_limit, stock_name);
    char *stock_end = stock == NULL ? NULL : entry_end(stock);
    char *original = entry_start(section, section_limit, original_name);
    char *original_end = original == NULL ? NULL : entry_end(original);
    char *extra = extra_name == NULL
        ? NULL
        : entry_start(section, section_limit, extra_name);
    char *extra_end = extra == NULL ? NULL : entry_end(extra);
    if (stock == NULL || stock_end == NULL) {
        errno = EINVAL;
        return false;
    }
    char *expected_extra = original == NULL ? stock_end : original_end;
    if (extra != NULL && extra != expected_extra) {
        if (!replace_range(contents_pointer, length_pointer, extra, extra_end, "", 0)) {
            return false;
        }
        return patch_group(
            contents_pointer, length_pointer, marker, stock_name, original_name,
            extra_name, patched_inode, original_inode, extra_inode
        );
    }

    const char *metadata_entry = original == NULL ? stock : original;
    const char *metadata_limit = original == NULL ? stock_end : original_end;
    char digest[128];
    char sibling[64];
    if (metadata_limit == NULL ||
        !copy_token(metadata_entry, metadata_limit, "siblingid=", sibling,
                    sizeof(sibling))) {
        errno = EINVAL;
        return false;
    }
    if (!copy_token(metadata_entry, metadata_limit, "xattrsdigest=", digest,
                    sizeof(digest))) {
        (void)snprintf(digest, sizeof(digest), "none.0");
    }

    char *replace_end = stock_end;
    if (original != NULL && original_end > replace_end) {
        replace_end = original_end;
    }
    if (extra != NULL && extra_end > replace_end) {
        replace_end = extra_end;
    }
    char replacement[1536];
    int replacement_length;
    if (extra_name == NULL) {
        replacement_length = snprintf(
            replacement,
            sizeof(replacement),
            "    %s \\\n"
            "                xattrsdigest=none.0 inode=%" PRIu64 " \\\n"
            "                siblingid=0\n"
            "    %s \\\n"
            "                xattrsdigest=%s inode=%" PRIu64 " \\\n"
            "                siblingid=%s\n",
            stock_name, patched_inode, original_name, digest, original_inode,
            sibling
        );
    } else {
        replacement_length = snprintf(
            replacement,
            sizeof(replacement),
            "    %s \\\n"
            "                xattrsdigest=none.0 inode=%" PRIu64 " \\\n"
            "                siblingid=0\n"
            "    %s \\\n"
            "                xattrsdigest=%s inode=%" PRIu64 " \\\n"
            "                siblingid=%s\n"
            "    %s \\\n"
            "                xattrsdigest=none.0 inode=%" PRIu64 " \\\n"
            "                siblingid=0\n",
            stock_name, patched_inode, original_name, digest, original_inode,
            sibling, extra_name, extra_inode
        );
    }
    return replacement_length > 0 &&
        replacement_length < (int)sizeof(replacement) &&
        replace_range(contents_pointer, length_pointer, stock, replace_end,
                      replacement, (size_t)replacement_length);
}

static bool patch_manifest(
    const char *manifest,
    const struct anchor_inodes *inodes
) {
    int input = open(manifest, O_RDONLY | O_CLOEXEC);
    if (input < 0) {
        return false;
    }
    struct stat information;
    if (fstat(input, &information) != 0 || information.st_size <= 0) {
        close(input);
        return false;
    }
    size_t length = (size_t)information.st_size;
    char *contents = malloc(length + 1);
    if (contents == NULL) {
        close(input);
        return false;
    }
    size_t offset = 0;
    while (offset < length) {
        ssize_t count = read(input, contents + offset, length - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            free(contents);
            close(input);
            return false;
        }
        offset += (size_t)count;
    }
    close(input);
    contents[length] = '\0';

    bool success = patch_group(
        &contents, &length, libexec_marker, "launchd_cache_loader",
        "launchd_cache_loader.srr", "surreal_loader",
        inodes->cache_loader, inodes->cache_loader_original, inodes->loader
    ) && patch_group(
        &contents, &length, xpc_marker, "launchd.plist",
        "launchd.plist.srr", NULL, inodes->launchd_cache,
        inodes->launchd_cache_original, 0
    );
    if (!success) {
        free(contents);
        return false;
    }

    char temporary[4096];
    if (snprintf(temporary, sizeof(temporary), "%s.surrealra1n", manifest) >=
        (int)sizeof(temporary)) {
        free(contents);
        errno = ENAMETOOLONG;
        return false;
    }
    int output = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                      information.st_mode & 07777);
    success = output >= 0 && write_all(output, contents, length);
    if (success) {
        (void)fchown(output, information.st_uid, information.st_gid);
        (void)fchmod(output, information.st_mode & 07777);
        success = fsync(output) == 0;
    }
    if (output >= 0) {
        close(output);
    }
    free(contents);
    if (success) {
        success = rename(temporary, manifest) == 0;
    }
    if (!success) {
        (void)unlink(temporary);
    }
    return success;
}

int main(int argc, char **argv) {
    const char *root = NULL;
    const char *manifest = NULL;
    for (int index = 1; index + 1 < argc; ++index) {
        if (strcmp(argv[index], "-p") == 0) {
            root = argv[index + 1];
        } else if (strcmp(argv[index], "-f") == 0) {
            manifest = argv[index + 1];
        }
    }
    struct anchor_inodes inodes = {0};
    if (root == NULL || manifest == NULL || !install_anchor(root, &inodes) ||
        !patch_manifest(manifest, &inodes)) {
        dprintf(STDERR_FILENO, "mtree wrapper: launchd cache patch failed: %d\n",
                errno);
        return 65;
    }
    dprintf(
        STDERR_FILENO,
        "SURREALRAIN_MTREE_ANCHOR_INODES=%" PRIu64 ":%" PRIu64 ":%" PRIu64
        ":%" PRIu64 ":%" PRIu64 "\n",
        inodes.cache_loader, inodes.cache_loader_original, inodes.loader,
        inodes.launchd_cache, inodes.launchd_cache_original
    );

    argv[0] = MTREE_REAL_PATH;
    execv(argv[0], argv);
    dprintf(STDERR_FILENO, "mtree wrapper: execv failed: %d\n", errno);
    return 127;
}
