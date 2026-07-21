#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const char purplebuddy_plist[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "  <key>SetupDone</key>\n"
    "  <true/>\n"
    "  <key>SetupFinishedAllSteps</key>\n"
    "  <true/>\n"
    "  <key>UserChoseLanguage</key>\n"
    "  <true/>\n"
    "  <key>UserChoseRegion</key>\n"
    "  <true/>\n"
    "</dict>\n"
    "</plist>\n";

static void console_log(const char *format, ...) {
    int console = open("/dev/console", O_WRONLY | O_CLOEXEC);
    if (console < 0) {
        return;
    }

    dprintf(console, "[surrealra1n skip-setup] ");
    va_list arguments;
    va_start(arguments, format);
    vdprintf(console, format, arguments);
    va_end(arguments);
    dprintf(console, "\n");
    close(console);
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

static bool directory_exists(const char *path) {
    struct stat information;
    return stat(path, &information) == 0 && S_ISDIR(information.st_mode);
}

static bool install_setup_markers(const char *preferences) {
    char marker[PATH_MAX];
    char plist[PATH_MAX];
    char temporary_plist[PATH_MAX];
    char mobile_directory[PATH_MAX];
    char log_path[PATH_MAX];
    const char suffix[] = "/Library/Preferences";
    size_t preferences_length = strlen(preferences);
    size_t suffix_length = strlen(suffix);

    if (snprintf(marker, sizeof(marker), "%s/.AppleSetupDone", preferences) >=
            (int)sizeof(marker) ||
        snprintf(plist, sizeof(plist), "%s/com.apple.purplebuddy.plist", preferences) >=
            (int)sizeof(plist) ||
        snprintf(temporary_plist, sizeof(temporary_plist), "%s.surrealra1n", plist) >=
            (int)sizeof(temporary_plist)) {
        return false;
    }

    int marker_file = open(marker, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (marker_file < 0) {
        return false;
    }
    (void)fchown(marker_file, 501, 501);
    (void)fchmod(marker_file, 0600);
    (void)fsync(marker_file);
    close(marker_file);

    int plist_file = open(
        temporary_plist, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600
    );
    if (plist_file < 0) {
        return false;
    }
    bool plist_written = write_all(
        plist_file, purplebuddy_plist, sizeof(purplebuddy_plist) - 1
    );
    (void)fchown(plist_file, 501, 501);
    (void)fchmod(plist_file, 0600);
    (void)fsync(plist_file);
    close(plist_file);
    if (!plist_written || rename(temporary_plist, plist) != 0) {
        (void)unlink(temporary_plist);
        return false;
    }

    if (preferences_length > suffix_length &&
        strcmp(preferences + preferences_length - suffix_length, suffix) == 0 &&
        preferences_length - suffix_length < sizeof(mobile_directory)) {
        memcpy(mobile_directory, preferences, preferences_length - suffix_length);
        mobile_directory[preferences_length - suffix_length] = '\0';
        if (snprintf(
                log_path,
                sizeof(log_path),
                "%s/Media/surrealra1n-skip-setup.log",
                mobile_directory
            ) < (int)sizeof(log_path)) {
            int log_file = open(
                log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644
            );
            if (log_file >= 0) {
                static const char message[] =
                    "Setup Assistant completion markers installed; activation untouched.\n";
                (void)write_all(log_file, message, sizeof(message) - 1);
                (void)fchown(log_file, 501, 501);
                (void)fchmod(log_file, 0644);
                (void)fsync(log_file);
                close(log_file);
            }
        }
    }

    sync();
    return true;
}

int main(void) {
    bool reported_success = false;
    const char *preferences_override = getenv("SURREALRA1N_PREFERENCES_PATH");
    const char *attempts_override = getenv("SURREALRA1N_MAX_ATTEMPTS");
    unsigned int maximum_attempts = 7200;
    if (attempts_override != NULL) {
        unsigned long parsed_attempts = strtoul(attempts_override, NULL, 10);
        if (parsed_attempts > 0 && parsed_attempts <= 7200) {
            maximum_attempts = (unsigned int)parsed_attempts;
        }
    }
    console_log("native helper started; waiting for the Data volume");

    for (unsigned int attempt = 0; attempt < maximum_attempts; ++attempt) {
        if (preferences_override != NULL && directory_exists(preferences_override)) {
            if (install_setup_markers(preferences_override) && !reported_success) {
                console_log("markers installed under %s", preferences_override);
                reported_success = true;
            }
            sleep(1);
            continue;
        }

        for (unsigned int mount_number = 1; mount_number <= 9; ++mount_number) {
            char preferences[PATH_MAX];
            const char *formats[] = {
                "/mnt%u/mobile/Library/Preferences",
            };

            for (size_t format_index = 0;
                 format_index < sizeof(formats) / sizeof(formats[0]);
                 ++format_index) {
                if (snprintf(
                        preferences,
                        sizeof(preferences),
                        formats[format_index],
                        mount_number
                    ) >= (int)sizeof(preferences) ||
                    !directory_exists(preferences)) {
                    continue;
                }

                if (install_setup_markers(preferences) && !reported_success) {
                    console_log("markers installed under %s", preferences);
                    reported_success = true;
                }
            }
        }
        sleep(1);
    }

    console_log("helper timeout reached");
    return 0;
}
