#define _GNU_SOURCE
#include "child_process.h"
#include "common.h"
#include "file_system.h"
#include "logging.h"
#include "networking.h"
#include "util.h"

#define STACK_SIZE (1024 * 1024)

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
  struct child_args args = {.argv = argv};

  // Allocate stack for child
  char *stack = malloc(STACK_SIZE);
  if (stack == NULL)
  {
    handle_error("malloc");
  }

  // Define namespaces for isolation
  int clone_flags =
      CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNET;

  // Create new process with namespaces
  pid_t pid =
      clone(child_function, stack + STACK_SIZE, clone_flags | SIGCHLD, &args);
  if (pid == -1)
  {
    handle_error("clone");
  }

  if (setup_networking(pid) != 0)
  {
    LOG("Warning: Failed to setup networking\n");
  }

  // Wait for child to finish
  int status;
  if (waitpid(pid, &status, 0) == -1)
  {
    handle_error("waitpid");
  }

  // Cleanup
  cleanup_networking();
  cleanup_container_root();
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