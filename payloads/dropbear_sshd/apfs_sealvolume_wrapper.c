#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *real_sealer =
    "/System/Library/Filesystems/apfs.fs/apfs_sealvolume.real";

int main(int argc, char **argv) {
    char **arguments = calloc((size_t)argc + 1, sizeof(*arguments));
    if (arguments == NULL) {
        dprintf(STDERR_FILENO, "apfs_sealvolume wrapper: calloc failed: %d\n", errno);
        return 126;
    }

    int output = 0;
    arguments[output++] = (char *)real_sealer;
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "-u") == 0) {
            if (index + 1 >= argc) {
                dprintf(STDERR_FILENO, "apfs_sealvolume wrapper: -u has no value\n");
                free(arguments);
                return 64;
            }
            ++index;
            continue;
        }
        arguments[output++] = argv[index];
    }
    arguments[output] = NULL;

    execv(arguments[0], arguments);
    dprintf(STDERR_FILENO, "apfs_sealvolume wrapper: execv failed: %d\n", errno);
    free(arguments);
    return 127;
}
