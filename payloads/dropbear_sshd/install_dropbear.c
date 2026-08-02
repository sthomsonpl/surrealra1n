#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/attr.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const char payload_root[] =
    "/usr/local/share/surrealra1n_dropbear/rootfs/var/jb";
static char persistent_log_path[PATH_MAX] = "";

static void console_log(const char *format, ...) {
    char message[2048];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (length < 0) {
        return;
    }

    int destinations[] = {
        open("/dev/console", O_WRONLY | O_CLOEXEC),
        persistent_log_path[0] != '\0'
            ? open(
                  persistent_log_path,
                  O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                  0644
              )
            : -1,
    };
    for (size_t index = 0; index < sizeof(destinations) / sizeof(destinations[0]); ++index) {
        if (destinations[index] < 0) {
            continue;
        }
        dprintf(destinations[index], "[surrealra1n dropbear] %s\n", message);
        close(destinations[index]);
    }
}

static bool path_is_directory(const char *path) {
    struct stat information;
    return stat(path, &information) == 0 && S_ISDIR(information.st_mode);
}

static bool path_exists(const char *path) {
    struct stat information;
    return lstat(path, &information) == 0;
}

static bool path_is_nonempty_file(const char *path) {
    struct stat information;
    return stat(path, &information) == 0 && S_ISREG(information.st_mode) &&
           information.st_size > 0;
}

static bool symlink_matches(const char *path, const char *expected) {
    char target[PATH_MAX];
    ssize_t length = readlink(path, target, sizeof(target) - 1);
    if (length < 0) {
        return false;
    }
    target[length] = '\0';
    return strcmp(target, expected) == 0;
}

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

static bool copy_regular_file(
    const char *source,
    const char *destination,
    const struct stat *information
) {
    char temporary[PATH_MAX];
    if (snprintf(temporary, sizeof(temporary), "%s.surrealra1n", destination) >=
        (int)sizeof(temporary)) {
        return false;
    }
    int input = open(source, O_RDONLY | O_CLOEXEC);
    if (input < 0) {
        return false;
    }
    int output = open(
        temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, information->st_mode & 07777
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
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            success = false;
            break;
        }
        if (!write_all(output, buffer, (size_t)count)) {
            success = false;
            break;
        }
    }
    if (success) {
        (void)fchown(output, information->st_uid, information->st_gid);
        (void)fchmod(output, information->st_mode & 07777);
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

static bool copy_tree(const char *source, const char *destination) {
    struct stat information;
    if (lstat(source, &information) != 0) {
        return false;
    }
    if (S_ISREG(information.st_mode)) {
        return copy_regular_file(source, destination, &information);
    }
    if (S_ISLNK(information.st_mode)) {
        char target[PATH_MAX];
        ssize_t length = readlink(source, target, sizeof(target) - 1);
        if (length < 0) {
            return false;
        }
        target[length] = '\0';
        (void)unlink(destination);
        return symlink(target, destination) == 0;
    }
    if (!S_ISDIR(information.st_mode)) {
        return true;
    }
    if (mkdir(destination, information.st_mode & 07777) != 0 && errno != EEXIST) {
        return false;
    }
    (void)chown(destination, information.st_uid, information.st_gid);
    (void)chmod(destination, information.st_mode & 07777);

    DIR *directory = opendir(source);
    if (directory == NULL) {
        return false;
    }
    bool success = true;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child_source[PATH_MAX];
        char child_destination[PATH_MAX];
        if (snprintf(child_source, sizeof(child_source), "%s/%s", source, entry->d_name) >=
                (int)sizeof(child_source) ||
            snprintf(
                child_destination,
                sizeof(child_destination),
                "%s/%s",
                destination,
                entry->d_name
            ) >= (int)sizeof(child_destination) ||
            !copy_tree(child_source, child_destination)) {
            success = false;
            break;
        }
    }
    closedir(directory);
    return success;
}

static bool remove_tree(const char *path) {
    struct stat information;
    if (lstat(path, &information) != 0) {
        return errno == ENOENT;
    }
    if (!S_ISDIR(information.st_mode) || S_ISLNK(information.st_mode)) {
        return unlink(path) == 0;
    }

    DIR *directory = opendir(path);
    if (directory == NULL) {
        return false;
    }
    bool success = true;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[PATH_MAX];
        if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >=
                (int)sizeof(child) ||
            !remove_tree(child)) {
            success = false;
            break;
        }
    }
    closedir(directory);
    return success && rmdir(path) == 0;
}

static bool make_directory_path(const char *path, mode_t mode) {
    char copy[PATH_MAX];
    if (snprintf(copy, sizeof(copy), "%s", path) >= (int)sizeof(copy)) {
        return false;
    }
    for (char *cursor = copy + 1; *cursor != '\0'; ++cursor) {
        if (*cursor != '/') {
            continue;
        }
        *cursor = '\0';
        if (mkdir(copy, mode) != 0 && errno != EEXIST) {
            return false;
        }
        *cursor = '/';
    }
    return mkdir(copy, mode) == 0 || errno == EEXIST;
}

static bool volume_name(const char *path, char *name, size_t name_size) {
    struct attrlist attributes;
    memset(&attributes, 0, sizeof(attributes));
    attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
    attributes.volattr = ATTR_VOL_INFO | ATTR_VOL_NAME;

    struct {
        uint32_t length;
        attrreference_t reference;
        char storage[NAME_MAX + 1];
    } buffer;
    memset(&buffer, 0, sizeof(buffer));
    if (getattrlist(path, &attributes, &buffer, sizeof(buffer), 0) != 0) {
        return false;
    }
    const char *value = (const char *)&buffer.reference +
                        buffer.reference.attr_dataoffset;
    size_t length = (size_t)buffer.reference.attr_length;
    if (length == 0 || length > sizeof(buffer.storage) || length > name_size) {
        return false;
    }
    memcpy(name, value, length);
    name[name_size - 1] = '\0';
    return true;
}

static bool mount_allows_execution(const char *path) {
    struct statfs filesystem;
    return statfs(path, &filesystem) == 0 &&
           (filesystem.f_flags & MNT_NOEXEC) == 0;
}

static bool namespace_name(const char *name) {
    size_t length = strlen(name);
    if (length == 36 && name[8] == '-' && name[13] == '-' &&
        name[18] == '-' && name[23] == '-') {
        return true;
    }
    if (length < 40 || length > 128) {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        char character = name[index];
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f') ||
              (character >= 'A' && character <= 'F'))) {
            return false;
        }
    }
    return true;
}

static bool contains_preboot_namespace(const char *mount_root) {
    DIR *directory = opendir(mount_root);
    if (directory == NULL) {
        return false;
    }
    bool found = false;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (!namespace_name(entry->d_name)) {
            continue;
        }
        char candidate[PATH_MAX];
        if (snprintf(candidate, sizeof(candidate), "%s/%s", mount_root, entry->d_name) <
                (int)sizeof(candidate) &&
            path_is_directory(candidate)) {
            found = true;
            break;
        }
    }
    closedir(directory);
    return found;
}

static bool select_preboot_base(
    const char *mount_root,
    char *restore_path,
    size_t restore_path_size,
    char *runtime_path,
    size_t runtime_path_size
) {
    DIR *directory = opendir(mount_root);
    if (directory == NULL) {
        return false;
    }
    char selected[NAME_MAX + 1] = "";
    time_t selected_time = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (!namespace_name(entry->d_name)) {
            continue;
        }
        char candidate[PATH_MAX];
        struct stat information;
        if (snprintf(candidate, sizeof(candidate), "%s/%s", mount_root, entry->d_name) >=
                (int)sizeof(candidate) ||
            stat(candidate, &information) != 0 || !S_ISDIR(information.st_mode)) {
            continue;
        }
        if (selected[0] == '\0' || information.st_mtime > selected_time) {
            (void)snprintf(selected, sizeof(selected), "%s", entry->d_name);
            selected_time = information.st_mtime;
        }
    }
    closedir(directory);

    if (selected[0] != '\0') {
        return snprintf(
                   restore_path,
                   restore_path_size,
                   "%s/%s/surrealra1n",
                   mount_root,
                   selected
               ) < (int)restore_path_size &&
               snprintf(
                   runtime_path,
                   runtime_path_size,
                   "/private/preboot/%s/surrealra1n",
                   selected
               ) < (int)runtime_path_size;
    }

    console_log("Preboot namespace is absent; using the volume root fallback");
    return snprintf(restore_path, restore_path_size, "%s/surrealra1n", mount_root) <
               (int)restore_path_size &&
           snprintf(
               runtime_path,
               runtime_path_size,
               "/private/preboot/surrealra1n"
           ) < (int)runtime_path_size;
}

static bool install_on_preboot(
    const char *data_root,
    const char *preboot_root
) {
    char restore_base[PATH_MAX];
    char runtime_base[PATH_MAX];
    char destination[PATH_MAX];
    char runtime_destination[PATH_MAX];
    char data_link[PATH_MAX];
    char temporary_link[PATH_MAX];
    char payload_etc[PATH_MAX];
    char preboot_data[PATH_MAX];
    char preboot_etc[PATH_MAX];
    char data_state_root[PATH_MAX];
    char data_state_etc[PATH_MAX];
    char runtime_state_root[PATH_MAX];
    char required_bash[PATH_MAX];
    char required_dropbear[PATH_MAX];
    char required_master_passwd[PATH_MAX];
    char password_database[PATH_MAX];
    char secure_password_database[PATH_MAX];
    if (!select_preboot_base(
            preboot_root,
            restore_base,
            sizeof(restore_base),
            runtime_base,
            sizeof(runtime_base)
        ) ||
        snprintf(destination, sizeof(destination), "%s/jb", restore_base) >=
            (int)sizeof(destination) ||
        snprintf(runtime_destination, sizeof(runtime_destination), "%s/jb", runtime_base) >=
            (int)sizeof(runtime_destination) ||
        snprintf(data_link, sizeof(data_link), "%s/jb", data_root) >=
            (int)sizeof(data_link) ||
        snprintf(
            temporary_link,
            sizeof(temporary_link),
            "%s/jb.surrealra1n-new",
            data_root
        ) >= (int)sizeof(temporary_link) ||
        snprintf(payload_etc, sizeof(payload_etc), "%s/etc", payload_root) >=
            (int)sizeof(payload_etc) ||
        snprintf(preboot_data, sizeof(preboot_data), "%s/data", destination) >=
            (int)sizeof(preboot_data) ||
        snprintf(preboot_etc, sizeof(preboot_etc), "%s/etc", destination) >=
            (int)sizeof(preboot_etc) ||
        snprintf(data_state_root, sizeof(data_state_root), "%s/surrealra1n", data_root) >=
            (int)sizeof(data_state_root) ||
        snprintf(data_state_etc, sizeof(data_state_etc), "%s/etc", data_state_root) >=
            (int)sizeof(data_state_etc) ||
        snprintf(
            runtime_state_root,
            sizeof(runtime_state_root),
            "/private/var/surrealra1n"
        ) >= (int)sizeof(runtime_state_root) ||
        snprintf(required_bash, sizeof(required_bash), "%s/usr/bin/bash", destination) >=
            (int)sizeof(required_bash) ||
        snprintf(
            required_dropbear,
            sizeof(required_dropbear),
            "%s/usr/sbin/dropbear",
            destination
        ) >= (int)sizeof(required_dropbear) ||
        snprintf(
            required_master_passwd,
            sizeof(required_master_passwd),
            "%s/master.passwd",
            data_state_etc
        ) >= (int)sizeof(required_master_passwd) ||
        snprintf(password_database, sizeof(password_database), "%s/pwd.db", data_state_etc) >=
            (int)sizeof(password_database) ||
        snprintf(
            secure_password_database,
            sizeof(secure_password_database),
            "%s/spwd.db",
            data_state_etc
        ) >= (int)sizeof(secure_password_database)) {
        return false;
    }

    if (path_is_nonempty_file(required_bash) &&
        path_is_nonempty_file(required_dropbear) &&
        path_is_nonempty_file(required_master_passwd) &&
        symlink_matches(preboot_data, runtime_state_root) &&
        symlink_matches(preboot_etc, "data/etc") &&
        symlink_matches(data_link, runtime_destination)) {
        console_log("payload and rootless Dropbear are already complete; skipping copy");
        return true;
    }

    console_log(
        "installing executable payload at %s (runtime %s)",
        destination,
        runtime_destination
    );
    if (!make_directory_path(restore_base, 0755)) {
        console_log("failed to create Preboot payload directory: %d (%s)", errno, strerror(errno));
        return false;
    }
    if (!copy_tree(payload_root, destination)) {
        console_log("failed to copy payload onto Preboot: %d (%s)", errno, strerror(errno));
        return false;
    }
    if (!make_directory_path(data_state_root, 0755) ||
        !copy_tree(payload_etc, data_state_etc)) {
        console_log("failed to prepare writable payload state on Data: %d (%s)", errno, strerror(errno));
        return false;
    }
    (void)unlink(password_database);
    (void)unlink(secure_password_database);
    console_log("invalidated the previous rootless Dropbear password database");
    if (path_exists(preboot_data) && !remove_tree(preboot_data)) {
        console_log("failed to replace the payload Data link: %d (%s)", errno, strerror(errno));
        return false;
    }
    if (symlink(runtime_state_root, preboot_data) != 0 ||
        !symlink_matches(preboot_data, runtime_state_root)) {
        console_log("failed to activate the writable payload Data link: %d (%s)", errno, strerror(errno));
        return false;
    }
    if (path_exists(preboot_etc) && !remove_tree(preboot_etc)) {
        console_log("failed to replace Preboot etc with writable state link: %d (%s)", errno, strerror(errno));
        return false;
    }
    if (symlink("data/etc", preboot_etc) != 0) {
        console_log("failed to link Preboot etc to writable Data state: %d (%s)", errno, strerror(errno));
        return false;
    }
    if (!symlink_matches(preboot_etc, "data/etc")) {
        console_log("writable etc link verification failed");
        return false;
    }
    console_log("payload data -> %s; etc -> data/etc (writable state)", runtime_state_root);

    (void)unlink(temporary_link);
    if (symlink(runtime_destination, temporary_link) != 0) {
        console_log("failed to create the Data link: %d (%s)", errno, strerror(errno));
        return false;
    }
    if (path_exists(data_link) && !remove_tree(data_link)) {
        console_log("failed to replace old Data /jb: %d (%s)", errno, strerror(errno));
        (void)unlink(temporary_link);
        return false;
    }
    if (rename(temporary_link, data_link) != 0) {
        console_log("failed to activate the Data link: %d (%s)", errno, strerror(errno));
        (void)unlink(temporary_link);
        return false;
    }
    if (!symlink_matches(data_link, runtime_destination)) {
        console_log("Data /jb link verification failed");
        return false;
    }
    console_log(
        "activated Data /jb with writable password database and host-key state"
    );

    return true;
}

static bool detect_preboot(const char *candidate, const char *data_root) {
    if (!path_is_directory(candidate) ||
        (data_root[0] != '\0' && strcmp(data_root, candidate) == 0)) {
        return false;
    }

    char name[NAME_MAX + 1] = "";
    bool named_preboot = volume_name(candidate, name, sizeof(name)) &&
                         strcasecmp(name, "Preboot") == 0;
    bool structured_preboot = contains_preboot_namespace(candidate);
    if (!named_preboot && !structured_preboot) {
        return false;
    }
    if (!mount_allows_execution(candidate)) {
        console_log("refusing noexec Preboot mount at %s", candidate);
        return false;
    }
    console_log(
        "found executable Preboot at %s (volume=%s, detection=%s)",
        candidate,
        name[0] != '\0' ? name : "unknown",
        named_preboot ? "APFS name" : "namespace"
    );
    return true;
}

int main(void) {
    console_log("installer started; waiting for Data and executable Preboot");
    char data_root[32] = "";
    char preboot_root[32] = "";
    for (unsigned int attempt = 0; attempt < 7200; ++attempt) {
        for (unsigned int mount_number = 1; mount_number <= 9; ++mount_number) {
            char mount_root[32];
            char mobile[PATH_MAX];
            (void)snprintf(mount_root, sizeof(mount_root), "/mnt%u", mount_number);
            (void)snprintf(mobile, sizeof(mobile), "%s/mobile", mount_root);

            if (data_root[0] == '\0' && path_is_directory(mobile)) {
                (void)snprintf(data_root, sizeof(data_root), "%s", mount_root);
                (void)snprintf(
                    persistent_log_path,
                    sizeof(persistent_log_path),
                    "%s/mobile/Media/surreal-install.log",
                    data_root
                );
                console_log("found restored Data at %s", data_root);
            }
            if (preboot_root[0] == '\0' && detect_preboot(mount_root, data_root)) {
                (void)snprintf(preboot_root, sizeof(preboot_root), "%s", mount_root);
            }
        }

        if (preboot_root[0] == '\0' &&
            detect_preboot("/private/preboot", data_root)) {
            (void)snprintf(
                preboot_root,
                sizeof(preboot_root),
                "%s",
                "/private/preboot"
            );
        }

        if (data_root[0] != '\0' && preboot_root[0] != '\0' &&
            install_on_preboot(data_root, preboot_root)) {
            sync();
            console_log("persistent Dropbear Preboot installation completed");
            return 0;
        }
        sleep(1);
    }
    console_log(
        "installer timeout (Data=%s, Preboot=%s)",
        data_root[0] != '\0' ? data_root : "missing",
        preboot_root[0] != '\0' ? preboot_root : "missing"
    );
    return 1;
}
