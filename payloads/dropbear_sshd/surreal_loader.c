#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAXIMUM_SERVICES 64

static const char services_directory[] =
    "/var/jb/Library/SurrealLoader/Services";
static const char bash_path[] = "/var/jb/usr/bin/bash";
static const char dropbear_path[] = "/var/jb/usr/sbin/dropbear";
static const char writable_data_path[] = "/var/jb/data";
static const char lock_path[] = "/private/var/run/surreal-loader.lock";
static const char log_path[] = "/private/var/log/surreal-loader.log";
static const char media_log_path[] =
    "/private/var/mobile/Media/surreal-loader.log";
static const char services_log_path[] =
    "/private/var/mobile/Media/surreal-loader-services.log";

struct service {
    char name[NAME_MAX + 1];
    char path[PATH_MAX];
    pid_t process;
    time_t restart_after;
};

static struct service services[MAXIMUM_SERVICES];
static size_t service_count = 0;

static void loader_log(const char *format, ...) {
    char message[2048];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (length < 0) {
        return;
    }

    int descriptors[] = {
        open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644),
        open(media_log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644),
        open("/dev/console", O_WRONLY | O_CLOEXEC),
    };
    for (size_t index = 0; index < sizeof(descriptors) / sizeof(descriptors[0]); ++index) {
        if (descriptors[index] < 0) {
            continue;
        }
        dprintf(descriptors[index], "[SurrealLoader] %s\n", message);
        close(descriptors[index]);
    }
}

static bool regular_executable(const char *path) {
    struct stat information;
    return stat(path, &information) == 0 && S_ISREG(information.st_mode) &&
           (information.st_mode & 0111) != 0;
}

static void log_payload_mount(void) {
    char target[PATH_MAX];
    ssize_t length = readlink("/var/jb", target, sizeof(target) - 1);
    if (length < 0) {
        loader_log("/var/jb is not a symlink: %d (%s)", errno, strerror(errno));
    } else {
        target[length] = '\0';
        loader_log("/var/jb -> %s", target);
    }

    struct statfs filesystem;
    if (statfs(dropbear_path, &filesystem) != 0) {
        loader_log("could not inspect the payload mount: %d (%s)", errno, strerror(errno));
        return;
    }
    loader_log(
        "payload mount=%s source=%s flags=0x%lx noexec=%s",
        filesystem.f_mntonname,
        filesystem.f_mntfromname,
        (unsigned long)filesystem.f_flags,
        (filesystem.f_flags & MNT_NOEXEC) != 0 ? "yes" : "no"
    );

    if (statfs(writable_data_path, &filesystem) != 0) {
        loader_log("could not inspect writable payload data: %d (%s)", errno, strerror(errno));
        return;
    }
    loader_log(
        "payload data mount=%s source=%s flags=0x%lx readonly=%s",
        filesystem.f_mntonname,
        filesystem.f_mntfromname,
        (unsigned long)filesystem.f_flags,
        (filesystem.f_flags & MNT_RDONLY) != 0 ? "yes" : "no"
    );
}

static struct service *find_service(const char *name) {
    for (size_t index = 0; index < service_count; ++index) {
        if (strcmp(services[index].name, name) == 0) {
            return &services[index];
        }
    }
    return NULL;
}

static void discover_services(void) {
    DIR *directory = opendir(services_directory);
    if (directory == NULL) {
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] == '.' || find_service(entry->d_name) != NULL ||
            service_count >= MAXIMUM_SERVICES) {
            continue;
        }
        struct service *service = &services[service_count];
        if (snprintf(
                service->path,
                sizeof(service->path),
                "%s/%s",
                services_directory,
                entry->d_name
            ) >= (int)sizeof(service->path) ||
            !regular_executable(service->path)) {
            continue;
        }
        (void)snprintf(service->name, sizeof(service->name), "%s", entry->d_name);
        service->process = 0;
        service->restart_after = 0;
        ++service_count;
        loader_log("discovered service %s", service->name);
    }
    closedir(directory);
}

static void reap_services(void) {
    int status = 0;
    pid_t process;
    while ((process = waitpid(-1, &status, WNOHANG)) > 0) {
        for (size_t index = 0; index < service_count; ++index) {
            if (services[index].process != process) {
                continue;
            }
            if (WIFEXITED(status)) {
                loader_log(
                    "service %s exited with status %d",
                    services[index].name,
                    WEXITSTATUS(status)
                );
            } else if (WIFSIGNALED(status)) {
                loader_log(
                    "service %s terminated by signal %d",
                    services[index].name,
                    WTERMSIG(status)
                );
            }
            services[index].process = 0;
            services[index].restart_after = time(NULL) + 10;
            break;
        }
    }
}

static void start_services(void) {
    time_t now = time(NULL);
    for (size_t index = 0; index < service_count; ++index) {
        struct service *service = &services[index];
        if (service->process > 0 || now < service->restart_after ||
            !regular_executable(service->path)) {
            continue;
        }

        pid_t child = fork();
        if (child == 0) {
            int service_log = open(
                services_log_path,
                O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                0644
            );
            if (service_log >= 0) {
                (void)dup2(service_log, STDOUT_FILENO);
                (void)dup2(service_log, STDERR_FILENO);
                close(service_log);
            }
            (void)setenv(
                "PATH",
                "/var/jb/usr/bin:/var/jb/usr/sbin:/usr/bin:/bin:/usr/sbin:/sbin",
                1
            );
            (void)setenv("DYLD_LIBRARY_PATH", "/var/jb/usr/lib", 1);
            (void)setenv("HOME", "/var/root", 1);
            execl(bash_path, "bash", service->path, (char *)NULL);
            dprintf(
                STDERR_FILENO,
                "[SurrealLoader] bash exec failed for %s: %d (%s)\n",
                service->name,
                errno,
                strerror(errno)
            );
            _exit(127);
        }
        if (child < 0) {
            loader_log("could not start service %s: %d", service->name, errno);
            service->restart_after = now + 10;
            continue;
        }
        service->process = child;
        loader_log("started service %s as pid %d", service->name, child);
    }
}

int main(void) {
    int lock = open(lock_path, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (lock < 0) {
        loader_log("could not open the process lock: %d", errno);
        return 1;
    }
    if (flock(lock, LOCK_EX | LOCK_NB) != 0) {
        close(lock);
        return 0;
    }

    (void)ftruncate(lock, 0);
    dprintf(lock, "%d\n", getpid());
    loader_log("started as pid %d; waiting for the Preboot payload", getpid());

    bool payload_mount_logged = false;
    for (;;) {
        if (regular_executable(bash_path)) {
            if (!payload_mount_logged) {
                log_payload_mount();
                payload_mount_logged = true;
            }
            discover_services();
            reap_services();
            start_services();
        }
        sleep(5);
    }
}
