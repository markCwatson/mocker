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
void copy_directory(const char *src, const char *dst);
void setup_filesystem(void);
void cleanup_filesystem(void);
int child_function(void *arg);

// Error handling function
void handle_error(const char *msg) {
  perror(msg);
  exit(EXIT_FAILURE);
}

// Filesystem functions
void copy_directory(const char *src, const char *dst) {
  DIR *dir = opendir(src);
  if (!dir) {
    return;
  }

  mkdir(dst, 0755);

  struct dirent *entry;
  char src_path[PATH_MAX];
  char dst_path[PATH_MAX];

  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;

    snprintf(src_path, sizeof(src_path), "%s/%s", src, entry->d_name);
    snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, entry->d_name);

    struct stat statbuf;
    if (lstat(src_path, &statbuf) == -1) continue;

    if (S_ISDIR(statbuf.st_mode)) {
      copy_directory(src_path, dst_path);
    } else if (S_ISREG(statbuf.st_mode)) {
      FILE *src_file = fopen(src_path, "rb");
      if (!src_file) continue;

      FILE *dst_file = fopen(dst_path, "wb");
      if (!dst_file) {
        fclose(src_file);
        continue;
      }

      char buffer[8192];
      size_t size;
      while ((size = fread(buffer, 1, sizeof(buffer), src_file)) > 0) {
        fwrite(buffer, 1, size, dst_file);
      }

      fclose(src_file);
      fclose(dst_file);
      chmod(dst_path, statbuf.st_mode & 0777);
    } else if (S_ISLNK(statbuf.st_mode)) {
      char link_target[PATH_MAX];
      ssize_t len = readlink(src_path, link_target, sizeof(link_target) - 1);
      if (len != -1) {
        link_target[len] = '\0';
        symlink(link_target, dst_path);
      }
    }
  }
  closedir(dir);
}

void setup_filesystem(void) {
  // Make the mount namespace private
  if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == -1) {
    handle_error("mount private");
  }

  // Core directories to copy from host
  const char *base_dirs[] = {"bin", "etc", "lib", "usr", "sbin", NULL};

  printf("Setting up container root filesystem...\n");
  struct stat st;

  // Copy core directories from host
  for (const char **dir = base_dirs; *dir != NULL; dir++) {
    char src_path[PATH_MAX];
    char dst_path[PATH_MAX];
    snprintf(src_path, sizeof(src_path), "/%s", *dir);
    snprintf(dst_path, sizeof(dst_path), "%s/%s", CONTAINER_ROOT, *dir);

    if (stat(src_path, &st) == 0) {
      printf("  %s exists\n", src_path);
    } else {
      printf("  %s does not exist: %s\n", src_path, strerror(errno));
    }

    copy_directory(src_path, dst_path);

    if (stat(dst_path, &st) == 0) {
      printf("  Created %s\n", dst_path);
    } else {
      printf("  Failed to create %s: %s\n", dst_path, strerror(errno));
    }
  }

  // Create special directories
  const char *dirs[] = {CONTAINER_ROOT "/proc", CONTAINER_ROOT "/sys",
                        CONTAINER_ROOT "/dev", CONTAINER_ROOT "/tmp", NULL};

  for (const char **dir = dirs; *dir != NULL; dir++) {
    if (mkdir(*dir, 0755) && errno != EEXIST) {
      handle_error("mkdir");
    }
  }

  // Mount special filesystems
  if (mount("proc", CONTAINER_ROOT "/proc", "proc", 0, NULL) == -1) {
    handle_error("mount proc");
  }
  if (mount("sysfs", CONTAINER_ROOT "/sys", "sysfs", 0, NULL) == -1) {
    handle_error("mount sysfs");
  }
  if (mount("devtmpfs", CONTAINER_ROOT "/dev", "devtmpfs", 0, NULL) == -1) {
    handle_error("mount devtmpfs");
  }
}

void cleanup_filesystem(void) {
  // Unmount special filesystems
  umount(CONTAINER_ROOT "/dev");
  umount(CONTAINER_ROOT "/sys");
  umount(CONTAINER_ROOT "/proc");

  // Remove directories
  rmdir(CONTAINER_ROOT "/dev");
  rmdir(CONTAINER_ROOT "/sys");
  rmdir(CONTAINER_ROOT "/proc");
  rmdir(CONTAINER_ROOT "/tmp");
  rmdir(CONTAINER_ROOT);
}

int child_function(void *arg) {
  struct child_args *args = (struct child_args *)arg;

  printf("Setting up container root filesystem...\n");
  setup_filesystem();

  printf("Changing root...\n");
  if (chroot(CONTAINER_ROOT) == -1) {
    handle_error("chroot");
  }

  if (chdir("/") == -1) {
    handle_error("chdir");
  }

  // Execute the command
  char **argv = args->argv;
  if (execvp(argv[3], &argv[3]) == -1) {
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

  // Create container root directory
  if (mkdir(CONTAINER_ROOT, 0755) && errno != EEXIST) {
    handle_error("mkdir");
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
  cleanup_filesystem();
  free(stack);

  // Report exit status
  if (WIFEXITED(status)) {
    printf("Container exited with status %d\n", WEXITSTATUS(status));
  } else if (WIFSIGNALED(status)) {
    printf("Container killed by signal %d\n", WTERMSIG(status));
  }

  return 0;
}