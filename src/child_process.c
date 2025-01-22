#include "child_process.h"
#include "file_system.h"
#include "common.h"
#include "logging.h"
#include "util.h"

int child_function(void *arg)
{
    struct child_args *args = (struct child_args *)arg;

    LOG("Setting hostname...\n");
    sethostname("mocker", 6);

    LOG("Setting up container root...\n");
    setup_container_root();

    LOG("Changing root...\n");
    if (chroot(CONTAINER_ROOT) == -1)
    {
        handle_error("chroot");
    }

    if (chdir("/") == -1)
    {
        handle_error("chdir");
    }

    char **argv = args->argv;
    LOG("Attempting to execute: %s\n", argv[3]);
    if (execvp(argv[3], &argv[3]) == -1)
    {
        LOG("execvp failed: %s\n", strerror(errno));
        handle_error("execvp");
    }

    return 0;
}
