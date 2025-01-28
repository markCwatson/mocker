#define _GNU_SOURCE
#include "child_process.h"
#include "common.h"
#include "file_system.h"
#include "logging.h"
#include "networking/networking.h"
#include "util.h"
#include "cgroup.h"

#define STACK_SIZE (1024 * 1024)
// Define namespaces for isolation
#define CLONE_FLAGS (CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNET)

int main(int argc, char *argv[])
{
  disable_buffering();

  if (argc < 4)
  {
    fprintf(stderr, "Usage: %s run <image> <command> [args...]\n", argv[0]);
    exit(1);
  }

  if (strcmp(argv[1], "run") != 0)
  {
    fprintf(stderr, "Unknown command: %s\n", argv[1]);
    exit(1);
  }

  // Setup child arguments
  struct child_args args = {
      .argv = argv,
  };

  // Allocate stack for child
  char *stack = malloc(STACK_SIZE);
  if (stack == NULL)
  {
    handle_error("malloc");
  }

  // Create new process with namespaces
  pid_t child_pid = clone(child_function, stack + STACK_SIZE, CLONE_FLAGS | SIGCHLD, &args);
  if (child_pid == -1)
  {
    handle_error("clone");
  }

  // cgroup setup
  if (setup_cgroup(child_pid) != 0)
  {
    LOG("[MAIN] Warning: Failed to setup cgroup\n");
    kill(child_pid, SIGKILL);
    free(stack);
    handle_error("setup_cgroup");
  }
  else
  {
    LOG("[MAIN] Cgroup setup complete\n");
  }

  // let filesystem setup complete so we can copy /etc/resolv.conf
  usleep(50000);

  // Now setup networking
  LOG("[MAIN] Setting up networking...\n");
  if (setup_networking(child_pid) != 0)
  {
    LOG("[MAIN] Warning: Failed to setup networking\n");
    kill(child_pid, SIGKILL);
    cleanup_container_root();
    cleanup_cgroup();
    free(stack);
    handle_error("setup_networking");
  }
  else
  {
    LOG("[MAIN] Network setup complete\n");
  }

  // Wait for child to finish
  int status;
  if (waitpid(child_pid, &status, 0) == -1)
  {
    handle_error("waitpid");
  }

  cleanup_networking();
  cleanup_container_root();
  cleanup_cgroup();
  free(stack);

  // Report exit status
  if (WIFEXITED(status))
  {
    LOG("Container exited with status %d\n", WEXITSTATUS(status));
  }
  else if (WIFSIGNALED(status))
  {
    LOG("Container killed by signal %d\n", WTERMSIG(status));
  }

  return 0;
}