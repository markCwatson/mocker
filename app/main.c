#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ROOT "/tmp/container-root"

// Structure to pass arguments to child_function
struct child_args {
  char **argv;
};

// Forward declarations
void handle_error(const char *msg);
void setup_container_root(void);
void cleanup_container_root(void);
int child_function(void *arg);

// Error handling function
void handle_error(const char *msg) {
  perror(msg);
  exit(EXIT_FAILURE);
}

void setup_container_root(void) {
  printf("Creating minimal container root at %s\n", CONTAINER_ROOT);

  // Create basic directory structure
  const char *dirs[] = {CONTAINER_ROOT,         CONTAINER_ROOT "/bin",
                        CONTAINER_ROOT "/proc", CONTAINER_ROOT "/sys",
                        CONTAINER_ROOT "/dev",  NULL};

  // Clean up any existing container root
  char cmd[PATH_MAX];
  snprintf(cmd, sizeof(cmd), "rm -rf %s", CONTAINER_ROOT);
  system(cmd);

  // Create directories
  for (const char **dir = dirs; *dir != NULL; dir++) {
    printf("Creating directory %s\n", *dir);
    if (mkdir(*dir, 0755) && errno != EEXIST) {
      printf("Failed to create %s: %s\n", *dir, strerror(errno));
      handle_error("mkdir");
    }
  }

  snprintf(cmd, sizeof(cmd),
           "cp /bin/busybox %s/bin/busybox && chmod +x %s/bin/busybox",
           CONTAINER_ROOT, CONTAINER_ROOT);
  if (system(cmd) != 0) {
    fprintf(stderr, "Failed to setup busybox!\n");
    exit(1);
  }

  // Create essential command symlinks
  printf("Creating symlinks...\n");
  const char *commands[] = {"sh",    "ls",   "ps",  "mount", "umount",
                            "mkdir", "echo", "cat", "pwd",   NULL};

  // Change to the bin directory for creating symlinks
  char old_pwd[PATH_MAX];
  getcwd(old_pwd, sizeof(old_pwd));

  char bin_path[PATH_MAX];
  snprintf(bin_path, sizeof(bin_path), "%s/bin", CONTAINER_ROOT);
  chdir(bin_path);

  for (const char **cmd_ptr = commands; *cmd_ptr != NULL; cmd_ptr++) {
    if (symlink("busybox", *cmd_ptr) != 0 && errno != EEXIST) {
      printf("Warning: Failed to create symlink for %s: %s\n", *cmd_ptr,
             strerror(errno));
    }
  }

  chdir(old_pwd);

  // Mount essential filesystems
  const struct {
    const char *source;
    const char *target;
    const char *type;
    unsigned long flags;
  } mounts[] = {{"proc", CONTAINER_ROOT "/proc", "proc", 0},
                {"sysfs", CONTAINER_ROOT "/sys", "sysfs", 0},
                {"devtmpfs", CONTAINER_ROOT "/dev", "devtmpfs", 0},
                {NULL, NULL, NULL, 0}};

  for (int i = 0; mounts[i].source != NULL; i++) {
    printf("Mounting %s at %s\n", mounts[i].source, mounts[i].target);
    if (mount(mounts[i].source, mounts[i].target, mounts[i].type,
              mounts[i].flags, NULL) == -1) {
      printf("Warning: Could not mount %s: %s\n", mounts[i].target,
             strerror(errno));
    }
  }

  // Print busybox info
  printf("\nBusybox binary information:\n");
  snprintf(cmd, sizeof(cmd), "file %s/bin/busybox", CONTAINER_ROOT);
  system(cmd);
}

void cleanup_container_root(void) {
  printf("Cleaning up container root...\n");

  // Unmount special filesystems in reverse order
  const char *mounts[] = {CONTAINER_ROOT "/dev", CONTAINER_ROOT "/sys",
                          CONTAINER_ROOT "/proc", NULL};

  for (const char **mount_point = mounts; *mount_point != NULL; mount_point++) {
    printf("Unmounting %s...\n", *mount_point);
    if (umount2(*mount_point, MNT_DETACH) != 0) {
      printf("Warning: Failed to unmount %s: %s\n", *mount_point,
             strerror(errno));
    }
  }

  // Remove entire container root with all contents
  char cmd[PATH_MAX];
  snprintf(cmd, sizeof(cmd), "rm -rf %s", CONTAINER_ROOT);
  printf("Removing container root directory...\n");
  if (system(cmd) != 0) {
    printf("Warning: Failed to remove container root: %s\n", strerror(errno));
  }
}

// Function that runs inside the container
int child_function(void *arg) {
  struct child_args *args = (struct child_args *)arg;

  printf("Setting up container root...\n");
  setup_container_root();

  printf("Changing root...\n");
  if (chroot(CONTAINER_ROOT) == -1) {
    handle_error("chroot");
  }

  if (chdir("/") == -1) {
    handle_error("chdir");
  }

  char **argv = args->argv;
  printf("Attempting to execute: %s\n", argv[3]);
  if (execvp(argv[3], &argv[3]) == -1) {
    printf("execvp failed: %s\n", strerror(errno));
    handle_error("execvp");
  }

  return 0;
}

int main(int argc, char *argv[]) {
  setbuf(stdout, NULL);
  setbuf(stderr, NULL);

  if (argc < 4) {
    fprintf(stderr, "Usage: %s run <image> <command> [args...]\n", argv[0]);
    exit(1);
  }

  if (strcmp(argv[1], "run") != 0) {
    fprintf(stderr, "Unknown command: %s\n", argv[1]);
    exit(1);
  }

  // Setup child arguments
  struct child_args args = {.argv = argv};

  // Allocate stack for child
  char *stack = malloc(STACK_SIZE);
  if (stack == NULL) {
    handle_error("malloc");
  }

  // Define namespaces for isolation
  int clone_flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC;

  // Create new process with namespaces
  pid_t pid =
      clone(child_function, stack + STACK_SIZE, clone_flags | SIGCHLD, &args);

  if (pid == -1) {
    handle_error("clone");
  }

  // Wait for child to finish
  int status;
  if (waitpid(pid, &status, 0) == -1) {
    handle_error("waitpid");
  }

  // Cleanup
  cleanup_container_root();
  free(stack);

  // Report exit status
  if (WIFEXITED(status)) {
    printf("Container exited with status %d\n", WEXITSTATUS(status));
  } else if (WIFSIGNALED(status)) {
    printf("Container killed by signal %d\n", WTERMSIG(status));
  }

  return 0;
}