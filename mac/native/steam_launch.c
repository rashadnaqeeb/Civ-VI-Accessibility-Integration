// Native launcher in front of steam_launch.sh. The Steam launch option is
//   "<CAI folder>/steam_launch" %command%
// and this program does nothing but run steam_launch.sh, next to it, through
// /bin/sh with the same arguments and environment.
//
// Why it exists: Steam on Apple Silicon is a universal binary that runs
// natively, but it spawns the launch-option command with the CPU preference
// x86_64 first and arm64 second (posix_spawnattr_setbinpref_np in
// steamclient.dylib). For a "#!/bin/sh" script the kernel therefore execs the
// x86_64 slice of /bin/sh, which needs Rosetta; without Rosetta the spawn
// fails with EBADARCH and Steam logs "Failed to spawn process" and "OS Error
// 0". macOS 27 removed Rosetta on upgrade and a new Mac never has it. A binary
// with only an arm64 slice takes Steam's second preference and runs natively,
// so this launcher is built arm64-only; the shell it execs is then native too,
// and the script runs unchanged, with or without Rosetta.
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv) {
    char exe[PATH_MAX];
    uint32_t size = sizeof exe;
    if (_NSGetExecutablePath(exe, &size) != 0) {
        fprintf(stderr, "CAI steam_launch: executable path too long\n");
        return 1;
    }
    char real[PATH_MAX];
    if (realpath(exe, real) == NULL) {
        perror("CAI steam_launch: realpath");
        return 1;
    }
    char script[PATH_MAX];
    int n = snprintf(script, sizeof script, "%s/steam_launch.sh", dirname(real));
    if (n < 0 || (size_t)n >= sizeof script) {
        fprintf(stderr, "CAI steam_launch: script path too long\n");
        return 1;
    }
    char** args = calloc((size_t)argc + 2, sizeof *args);
    if (args == NULL) {
        perror("CAI steam_launch: calloc");
        return 1;
    }
    args[0] = "sh";
    args[1] = script;
    for (int i = 1; i < argc; i++) {
        args[i + 1] = argv[i];
    }
    args[argc + 1] = NULL;
    execv("/bin/sh", args);
    perror("CAI steam_launch: exec /bin/sh");
    return 1;
}
