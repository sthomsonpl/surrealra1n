#include <CoreFoundation/CoreFoundation.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const char default_source_path[] =
    "/usr/local/share/surrealra1n_setup_fix/disabled.plist";

static void console_log(const char *format, ...) {
    int console = open("/dev/console", O_WRONLY | O_CLOEXEC);
    if (console < 0) {
        return;
    }

    dprintf(console, "[surrealra1n setup-bypass] ");
    va_list arguments;
    va_start(arguments, format);
    vdprintf(console, format, arguments);
    va_end(arguments);
    dprintf(console, "\n");
    close(console);
}

static bool write_all(int descriptor, const UInt8 *buffer, CFIndex length) {
    while (length > 0) {
        ssize_t written = write(descriptor, buffer, (size_t)length);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            return false;
        }
        buffer += written;
        length -= written;
    }
    return true;
}

static CFPropertyListRef read_plist(
    const char *path,
    CFOptionFlags mutability,
    CFPropertyListFormat *format
) {
    int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return NULL;
    }

    struct stat information;
    if (fstat(descriptor, &information) != 0 || information.st_size < 0 ||
        (uint64_t)information.st_size > (uint64_t)INT_MAX) {
        close(descriptor);
        return NULL;
    }

    size_t length = (size_t)information.st_size;
    UInt8 *bytes = malloc(length == 0 ? 1 : length);
    if (bytes == NULL) {
        close(descriptor);
        return NULL;
    }

    size_t offset = 0;
    while (offset < length) {
        ssize_t result = read(descriptor, bytes + offset, length - offset);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            free(bytes);
            close(descriptor);
            return NULL;
        }
        offset += (size_t)result;
    }
    close(descriptor);

    CFDataRef data = CFDataCreate(kCFAllocatorDefault, bytes, (CFIndex)length);
    free(bytes);
    if (data == NULL) {
        return NULL;
    }

    CFErrorRef error = NULL;
    CFPropertyListRef plist = CFPropertyListCreateWithData(
        kCFAllocatorDefault, data, mutability, format, &error
    );
    if (error != NULL) {
        CFRelease(error);
    }
    CFRelease(data);
    return plist;
}

static bool directory_exists(const char *path) {
    struct stat information;
    return stat(path, &information) == 0 && S_ISDIR(information.st_mode);
}

static bool ensure_directory(const char *path) {
    if (mkdir(path, 0755) == 0 || errno == EEXIST) {
        return directory_exists(path);
    }
    return false;
}

struct merge_context {
    CFMutableDictionaryRef target;
    bool changed;
};

static void merge_entry(const void *key, const void *value, void *raw_context) {
    struct merge_context *context = raw_context;
    CFTypeRef current = CFDictionaryGetValue(context->target, key);
    if (current == NULL || !CFEqual(current, value)) {
        CFDictionarySetValue(context->target, key, value);
        context->changed = true;
    }
}

static bool write_plist_atomically(
    const char *target_path,
    CFPropertyListRef plist,
    CFPropertyListFormat format
) {
    char temporary_path[PATH_MAX];
    if (snprintf(
            temporary_path,
            sizeof(temporary_path),
            "%s.surrealra1n",
            target_path
        ) >= (int)sizeof(temporary_path)) {
        return false;
    }

    CFErrorRef error = NULL;
    CFDataRef output = CFPropertyListCreateData(
        kCFAllocatorDefault, plist, format, 0, &error
    );
    if (error != NULL) {
        CFRelease(error);
    }
    if (output == NULL) {
        return false;
    }

    int descriptor = open(
        temporary_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644
    );
    bool success = descriptor >= 0;
    if (success) {
        success = write_all(
            descriptor, CFDataGetBytePtr(output), CFDataGetLength(output)
        );
        if (fchown(descriptor, 0, 0) != 0 && errno != EPERM) {
            success = false;
        }
        if (fchmod(descriptor, 0644) != 0 || fsync(descriptor) != 0) {
            success = false;
        }
        if (close(descriptor) != 0) {
            success = false;
        }
    }
    CFRelease(output);

    if (!success || rename(temporary_path, target_path) != 0) {
        (void)unlink(temporary_path);
        return false;
    }
    return true;
}

static bool merge_disabled_plist(
    const char *data_root,
    CFDictionaryRef requested_values
) {
    char database_directory[PATH_MAX];
    char launchd_directory[PATH_MAX];
    char target_path[PATH_MAX];
    if (snprintf(
            database_directory,
            sizeof(database_directory),
            "%s/db",
            data_root
        ) >= (int)sizeof(database_directory) ||
        snprintf(
            launchd_directory,
            sizeof(launchd_directory),
            "%s/com.apple.xpc.launchd",
            database_directory
        ) >= (int)sizeof(launchd_directory) ||
        snprintf(
            target_path,
            sizeof(target_path),
            "%s/disabled.plist",
            launchd_directory
        ) >= (int)sizeof(target_path)) {
        return false;
    }

    if (!directory_exists(database_directory) ||
        !ensure_directory(launchd_directory)) {
        return false;
    }

    CFPropertyListFormat output_format = kCFPropertyListXMLFormat_v1_0;
    bool target_exists = access(target_path, F_OK) == 0;
    CFPropertyListRef existing = read_plist(
        target_path, kCFPropertyListMutableContainersAndLeaves, &output_format
    );
    CFMutableDictionaryRef target = NULL;
    if (existing != NULL && CFGetTypeID(existing) == CFDictionaryGetTypeID()) {
        target = (CFMutableDictionaryRef)existing;
    } else {
        if (existing != NULL) {
            CFRelease(existing);
        }
        if (target_exists) {
            return false;
        }
        target = CFDictionaryCreateMutable(
            kCFAllocatorDefault,
            0,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks
        );
        output_format = kCFPropertyListXMLFormat_v1_0;
    }
    if (target == NULL) {
        return false;
    }

    struct merge_context context = {target, false};
    CFDictionaryApplyFunction(requested_values, merge_entry, &context);
    bool success = !context.changed ||
        write_plist_atomically(target_path, target, output_format);
    CFRelease(target);
    return success;
}

int main(void) {
    const char *source_path = getenv("SURREALRAIN_SETUP_FIX_SOURCE");
    if (source_path == NULL || source_path[0] == '\0') {
        source_path = default_source_path;
    }

    CFPropertyListFormat source_format = kCFPropertyListXMLFormat_v1_0;
    CFPropertyListRef source = read_plist(
        source_path, kCFPropertyListImmutable, &source_format
    );
    if (source == NULL || CFGetTypeID(source) != CFDictionaryGetTypeID()) {
        if (source != NULL) {
            CFRelease(source);
        }
        console_log("invalid source plist: %s", source_path);
        return 1;
    }

    unsigned int maximum_attempts = 7200;
    const char *attempts_override = getenv("SURREALRAIN_MAX_ATTEMPTS");
    if (attempts_override != NULL) {
        unsigned long parsed_attempts = strtoul(attempts_override, NULL, 10);
        if (parsed_attempts > 0 && parsed_attempts <= 7200) {
            maximum_attempts = (unsigned int)parsed_attempts;
        }
    }

    const char *data_root_override = getenv("SURREALRAIN_DATA_ROOT");
    bool reported_success = false;
    console_log("waiting for the Data volume");
    for (unsigned int attempt = 0; attempt < maximum_attempts; ++attempt) {
        if (data_root_override != NULL && data_root_override[0] != '\0') {
            if (merge_disabled_plist(
                    data_root_override, (CFDictionaryRef)source
                ) && !reported_success) {
                console_log("merged disabled services under %s", data_root_override);
                reported_success = true;
            }
        } else {
            for (unsigned int mount_number = 1; mount_number <= 10; ++mount_number) {
                char data_root[PATH_MAX];
                char mobile_directory[PATH_MAX];
                if (snprintf(
                        data_root, sizeof(data_root), "/mnt%u", mount_number
                    ) >= (int)sizeof(data_root) ||
                    snprintf(
                        mobile_directory,
                        sizeof(mobile_directory),
                        "%s/mobile",
                        data_root
                    ) >= (int)sizeof(mobile_directory) ||
                    !directory_exists(mobile_directory)) {
                    continue;
                }
                if (merge_disabled_plist(data_root, (CFDictionaryRef)source) &&
                    !reported_success) {
                    console_log("merged disabled services under %s", data_root);
                    reported_success = true;
                }
            }
        }
        sleep(1);
    }

    CFRelease(source);
    console_log("helper timeout reached");
    return reported_success ? 0 : 1;
}
