#include "file_system.h"
#include "common.h"
#include "logging.h"
#include "util.h"

#include <sys/mount.h>
#include <sys/syscall.h>
#include <linux/fs.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

void setup_container_root(void)
{
    LOG("Creating minimal mocker root at %s\n", CONTAINER_ROOT);

    // Create basic directory structure
    const char *dirs[] = {
        CONTAINER_ROOT,
        CONTAINER_ROOT "/bin",
        CONTAINER_ROOT "/proc",
        CONTAINER_ROOT "/sys",
        CONTAINER_ROOT "/dev",
        NULL,
    };

    // Clean up any existing mocker root
    char cmd[PATH_MAX];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", CONTAINER_ROOT);
    system(cmd);

    // Create directories
    for (const char **dir = dirs; *dir != NULL; dir++)
    {
        LOG("Creating directory %s\n", *dir);
        if (mkdir(*dir, 0755) && errno != EEXIST)
        {
            LOG("Failed to create %s: %s\n", *dir, strerror(errno));
            handle_error("mkdir");
        }
    }

    snprintf(cmd, sizeof(cmd),
             "cp /bin/busybox %s/bin/busybox && chmod +x %s/bin/busybox",
             CONTAINER_ROOT, CONTAINER_ROOT);
    if (system(cmd) != 0)
    {
        fprintf(stderr, "Failed to setup busybox!\n");
        exit(1);
    }

    // Create essential command symlinks
    LOG("Creating symlinks...\n");
    const char *commands[] = {"sh", "ls", "ps", "mount", "umount", "mkdir", "echo", "cat", "pwd", NULL};

    // Change to the bin directory for creating symlinks
    char old_pwd[PATH_MAX];
    getcwd(old_pwd, sizeof(old_pwd));

    char bin_path[PATH_MAX];
    snprintf(bin_path, sizeof(bin_path), "%s/bin", CONTAINER_ROOT);
    chdir(bin_path);

    for (const char **cmd_ptr = commands; *cmd_ptr != NULL; cmd_ptr++)
    {
        if (symlink("busybox", *cmd_ptr) != 0 && errno != EEXIST)
        {
            LOG("Warning: Failed to create symlink for %s: %s\n", *cmd_ptr,
                strerror(errno));
        }
    }

    chdir(old_pwd);

    // Mount essential filesystems
    const struct
    {
        const char *source;
        const char *target;
        const char *type;
        unsigned long flags;
    } mounts[] = {
        {"proc", CONTAINER_ROOT "/proc", "proc", 0},
        {"sysfs", CONTAINER_ROOT "/sys", "sysfs", 0},
        {"devtmpfs", CONTAINER_ROOT "/dev", "devtmpfs", 0},
        {NULL, NULL, NULL, 0},
    };

    for (int i = 0; mounts[i].source != NULL; i++)
    {
        LOG("Mounting %s at %s\n", mounts[i].source, mounts[i].target);
        if (mount(mounts[i].source, mounts[i].target, mounts[i].type,
                  mounts[i].flags, NULL) == -1)
        {
            LOG("Warning: Could not mount %s: %s\n", mounts[i].target,
                strerror(errno));
        }
    }
}

void cleanup_container_root(void)
{
    LOG("Cleaning up mocker root...\n");

    // Unmount special filesystems in reverse order
    const char *mounts[] = {
        CONTAINER_ROOT "/dev",
        CONTAINER_ROOT "/sys",
        CONTAINER_ROOT "/proc",
        NULL,
    };

    for (const char **mount_point = mounts; *mount_point != NULL; mount_point++)
    {
        LOG("Unmounting %s...\n", *mount_point);
        if (umount2(*mount_point, MNT_DETACH) != 0)
        {
            LOG("Warning: Failed to unmount %s: %s\n", *mount_point, strerror(errno));
        }
    }

    // Remove entire mocker root with all contents
    char cmd[PATH_MAX];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", CONTAINER_ROOT);
    LOG("Removing mocker root directory...\n");
    if (system(cmd) != 0)
    {
        LOG("Warning: Failed to remove mocker root: %s\n", strerror(errno));
    }
}