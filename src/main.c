#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs.h"

extern const struct fuse_operations logfs_fuse_ops;

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--mode=ordered|writeback|journal] <image> <mountpoint> "
            "[FUSE options...]\n"
            "  --mode overrides the image's on-disk default data mode for this mount,\n"
            "  mirroring ext4's -o data= (see docs/DESIGN.md section 9).\n",
            argv0);
}

static int parse_mode(const char *s, logfs_data_mode_t *out) {
    if (strcmp(s, "ordered") == 0) {
        *out = LOGFS_MODE_ORDERED;
    } else if (strcmp(s, "writeback") == 0) {
        *out = LOGFS_MODE_WRITEBACK;
    } else if (strcmp(s, "journal") == 0) {
        *out = LOGFS_MODE_JOURNAL;
    } else {
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int mode_override = -1;
    int argi = 1;

    if (argi < argc && strncmp(argv[argi], "--mode=", 7) == 0) {
        logfs_data_mode_t m;
        if (parse_mode(argv[argi] + 7, &m) != 0) {
            fprintf(stderr, "%s: unrecognized --mode value '%s'\n", argv[0], argv[argi] + 7);
            usage(argv[0]);
            return 2;
        }
        mode_override = (int)m;
        argi++;
    }

    if (argc - argi < 2) {
        usage(argv[0]);
        return 2;
    }
    const char *image_path = argv[argi];
    argi++;
    /* argv[argi..] is the mountpoint plus any pass-through FUSE options. */

    static logfs_fs_t fs; /* outlives main(): fuse_main only returns at unmount */
    int rc = logfs_fs_mount(&fs, image_path, mode_override);
    if (rc != 0) {
        fprintf(stderr, "%s: mount failed on '%s': %s\n", argv[0], image_path, strerror(-rc));
        return 1;
    }

    /* Rebuild argv for fuse_main: program name + everything from the
     * mountpoint onward, dropping our own --mode/<image> arguments, which
     * FUSE's own option parser doesn't know about. */
    int fuse_argc = argc - argi + 1;
    char **fuse_argv = malloc((size_t)fuse_argc * sizeof(char *));
    if (fuse_argv == NULL) {
        logfs_fs_unmount(&fs);
        return 1;
    }
    fuse_argv[0] = argv[0];
    for (int i = argi; i < argc; i++) {
        fuse_argv[1 + (i - argi)] = argv[i];
    }

    int fuse_rc = fuse_main(fuse_argc, fuse_argv, &logfs_fuse_ops, &fs);

    free(fuse_argv);
    logfs_fs_unmount(&fs);
    return fuse_rc;
}
