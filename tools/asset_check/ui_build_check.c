/* The first-time setup's builder alone (tools/test/ui_build_check.py compares what it writes with the Python's).
 *
 *     ui_build_check <ps1 .cue> <pc .iso or folder | -> <out folder> */
#include <stdio.h>
#include <string.h>

#include "../../src/assets/uibuild.h"

static void progress(int pct, const char *what, void *ctx)
{
    (void)ctx;
    fprintf(stderr, "%3d%% %s\n", pct, what);
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "ui_build_check <ps1 .cue> <pc .iso or folder | -> <out>\n"); return 2; }
    if (!ui_build(argv[1], strcmp(argv[2], "-") ? argv[2] : NULL, argv[3], progress, NULL)) {
        fprintf(stderr, "failed: %s\n", ui_build_error());
        return 1;
    }
    return 0;
}
