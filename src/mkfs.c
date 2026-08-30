#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs.h"

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--mode=ordered|writeback|journal] [--journal-blocks=N] "
            "<image-path> <total-blocks>\n"
            "  block size is fixed at %u bytes; total-blocks * %u is the image size.\n"
            "  default mode is 'ordered' (ext4's default, and this project's\n"
            "  measured baseline — see docs/DESIGN.md " "\xc2\xa7" "7).\n",
            argv0, (unsigned)LOGFS_BLOCK_SIZE, (unsigned)LOGFS_BLOCK_SIZE);
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
    logfs_data_mode_t mode = LOGFS_MODE_ORDERED;
    uint64_t journal_blocks = 1024; /* 4MiB of journal at the default 4KiB block size */
    int argi = 1;

    while (argi < argc && strncmp(argv[argi], "--", 2) == 0) {
        if (strncmp(argv[argi], "--mode=", 7) == 0) {
            if (parse_mode(argv[argi] + 7, &mode) != 0) {
                fprintf(stderr, "%s: unrecognized --mode value '%s'\n", argv[0], argv[argi] + 7);
                usage(argv[0]);
                return 2;
            }
        } else if (strncmp(argv[argi], "--journal-blocks=", 17) == 0) {
            journal_blocks = strtoull(argv[argi] + 17, NULL, 10);
        } else {
            fprintf(stderr, "%s: unrecognized option '%s'\n", argv[0], argv[argi]);
            usage(argv[0]);
            return 2;
        }
        argi++;
    }

    if (argc - argi != 2) {
        usage(argv[0]);
        return 2;
    }
    const char *image_path = argv[argi];
    uint64_t total_blocks = strtoull(argv[argi + 1], NULL, 10);

    int rc = logfs_fs_format(image_path, total_blocks, journal_blocks, mode);
    if (rc != 0) {
        fprintf(stderr, "%s: format failed: %s\n", argv[0], strerror(-rc));
        return 1;
    }

    printf("formatted %s: %llu blocks (%llu MiB), %llu-block journal, default mode=%s\n",
           image_path, (unsigned long long)total_blocks,
           (unsigned long long)(total_blocks * LOGFS_BLOCK_SIZE / (1024 * 1024)),
           (unsigned long long)journal_blocks,
           mode == LOGFS_MODE_ORDERED ? "ordered" : mode == LOGFS_MODE_WRITEBACK ? "writeback" : "journal");
    return 0;
}
