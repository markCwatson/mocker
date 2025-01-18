#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/if.h>
#include <linux/rtnetlink.h>
#include <linux/sched.h>
#include <net/if.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>  // For time()
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ROOT "/tmp/container-root"

// Network configuration structure
struct netconfig {
  char container_veth[IFNAMSIZ];
  char host_veth[IFNAMSIZ];
  char container_ip[16];
  char host_ip[16];
  char netmask[16];
};

// Structure to pass arguments to child_function
struct child_args {
  char **argv;
  pid_t child_pid;
  struct netconfig net;
};

// Forward declarations of all functions
void handle_error(const char *msg);
void cleanup_network(const char *host_veth, const char *container_veth);
int create_veth_pair(const char *host_veth, const char *container_veth);
int move_to_netns(pid_t pid, const char *interface);
void setup_container_network(const struct netconfig *net);
void setup_host_network(const struct netconfig *net);
void copy_directory(const char *src, const char *dst);
void setup_filesystem(void);
void cleanup_filesystem(void);
int child_function(void *arg);

// Error handling function
void handle_error(const char *msg) {
  perror(msg);
  exit(EXIT_FAILURE);
}

// Network functions
void ensure_netns_dir() {
  // Create the netns directory if it doesn't exist
  if (mkdir("/var/run/netns", 0755) && errno != EEXIST) {
    printf("Warning: Could not create /var/run/netns: %s\n", strerror(errno));
  }

  // Mount it as a bind mount to make it permanent
  if (mount("none", "/var/run/netns", "tmpfs", 0, NULL) && errno != EBUSY) {
    printf("Warning: Could not mount /var/run/netns: %s\n", strerror(errno));
  }
}

void cleanup_network(const char *host_veth, const char *container_veth) {
  char cmd[200];

  // Print current network interfaces
  printf("Current network interfaces before cleanup:\n");
  system("ip link show");

  // Force delete any existing interfaces
  const char *cleanup_cmds[] = {"ip link set %s down 2>/dev/null",
                                "ip link del %s 2>/dev/null",
                                "ip link del %s type veth 2>/dev/null", NULL};

  // Try each cleanup command for both interfaces
  for (const char **cleanup_cmd = cleanup_cmds; *cleanup_cmd != NULL;
       cleanup_cmd++) {
    // Try host interface
    snprintf(cmd, sizeof(cmd), *cleanup_cmd, host_veth);
    printf("Executing: %s\n", cmd);
    system(cmd);

    // Try container interface
    snprintf(cmd, sizeof(cmd), *cleanup_cmd, container_veth);
    printf("Executing: %s\n", cmd);
    system(cmd);
  }

  // Verify cleanup
  printf("\nNetwork interfaces after cleanup:\n");
  system("ip link show");

  // Sleep to allow system to catch up
  usleep(500000);  // 500ms sleep
}

int create_veth_pair(const char *host_veth, const char *container_veth) {
  ensure_netns_dir();

  // Force cleanup any existing interfaces first
  cleanup_network(host_veth, container_veth);

  // Create netlink socket
  int sock = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
  if (sock < 0) {
    perror("Failed to open netlink socket");
    return -1;
  }

  // Create request structure
  struct {
    struct nlmsghdr hdr;
    struct ifinfomsg info;
    char buf[1024];
  } req = {
      .hdr.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg)),
      .hdr.nlmsg_type = RTM_NEWLINK,
      .hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL,
      .info.ifi_family = AF_UNSPEC,
  };

  // Add VETH specific attributes
  struct rtattr *linkinfo =
      (void *)((char *)&req + NLMSG_ALIGN(req.hdr.nlmsg_len));
  linkinfo->rta_type = IFLA_LINKINFO;

  // Add kind="veth"
  struct rtattr *kind =
      (void *)((char *)linkinfo + RTA_ALIGN(linkinfo->rta_len));
  kind->rta_type = IFLA_INFO_KIND;
  const char veth_kind[] = "veth";
  kind->rta_len = RTA_LENGTH(sizeof(veth_kind));
  memcpy(RTA_DATA(kind), veth_kind, sizeof(veth_kind));

  // Update lengths
  linkinfo->rta_len =
      RTA_ALIGN(sizeof(struct rtattr)) + RTA_ALIGN(kind->rta_len);
  req.hdr.nlmsg_len =
      NLMSG_ALIGN(req.hdr.nlmsg_len) + RTA_ALIGN(linkinfo->rta_len);

  // Add veth peer name
  struct rtattr *data =
      (void *)((char *)linkinfo + RTA_ALIGN(linkinfo->rta_len));
  data->rta_type = IFLA_INFO_DATA;

  struct rtattr *peer = (void *)((char *)data + RTA_ALIGN(data->rta_len));
  peer->rta_type = VETH_INFO_PEER;
  struct nlmsghdr *peer_hdr =
      (void *)((char *)peer + RTA_ALIGN(sizeof(struct rtattr)));
  peer_hdr->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));

  struct ifinfomsg *peer_info = NLMSG_DATA(peer_hdr);
  peer_info->ifi_family = AF_UNSPEC;

  struct rtattr *peer_name =
      (void *)((char *)peer_info + NLMSG_ALIGN(sizeof(struct ifinfomsg)));
  peer_name->rta_type = IFLA_IFNAME;
  peer_name->rta_len = RTA_LENGTH(strlen(container_veth) + 1);
  memcpy(RTA_DATA(peer_name), container_veth, strlen(container_veth) + 1);

  // Update all lengths
  peer->rta_len =
      RTA_ALIGN(sizeof(struct rtattr)) + NLMSG_ALIGN(sizeof(struct nlmsghdr)) +
      NLMSG_ALIGN(sizeof(struct ifinfomsg)) + RTA_ALIGN(peer_name->rta_len);
  data->rta_len = RTA_ALIGN(sizeof(struct rtattr)) + RTA_ALIGN(peer->rta_len);
  linkinfo->rta_len += RTA_ALIGN(data->rta_len);
  req.hdr.nlmsg_len += RTA_ALIGN(linkinfo->rta_len);

  // Add host interface name
  struct rtattr *name = (void *)((char *)&req + NLMSG_ALIGN(req.hdr.nlmsg_len));
  name->rta_type = IFLA_IFNAME;
  name->rta_len = RTA_LENGTH(strlen(host_veth) + 1);
  memcpy(RTA_DATA(name), host_veth, strlen(host_veth) + 1);
  req.hdr.nlmsg_len = NLMSG_ALIGN(req.hdr.nlmsg_len) + RTA_ALIGN(name->rta_len);

  // Send the netlink message
  struct sockaddr_nl addr = {.nl_family = AF_NETLINK};
  struct iovec iov = {&req, req.hdr.nlmsg_len};
  struct msghdr msg = {
      .msg_name = &addr,
      .msg_namelen = sizeof(addr),
      .msg_iov = &iov,
      .msg_iovlen = 1,
  };

  if (sendmsg(sock, &msg, 0) < 0) {
    perror("Failed to send netlink message");
    close(sock);
    return -1;
  }

  // Read response
  char buf[4096];
  struct iovec riov = {buf, sizeof(buf)};
  struct msghdr rmsg = {
      .msg_name = &addr,
      .msg_namelen = sizeof(addr),
      .msg_iov = &riov,
      .msg_iovlen = 1,
  };

  ssize_t recv_len = recvmsg(sock, &rmsg, 0);
  if (recv_len < 0) {
    perror("Failed to receive netlink response");
    close(sock);
    return -1;
  }

  // Set interfaces up using netlink
  struct ifreq ifr;
  int ctl_sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (ctl_sock < 0) {
    perror("Failed to create control socket");
    close(sock);
    return -1;
  }

  // Set host interface up
  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, host_veth, IFNAMSIZ - 1);
  if (ioctl(ctl_sock, SIOCGIFFLAGS, &ifr) < 0) {
    perror("Failed to get interface flags");
    close(ctl_sock);
    close(sock);
    return -1;
  }

  ifr.ifr_flags |= IFF_UP;
  if (ioctl(ctl_sock, SIOCSIFFLAGS, &ifr) < 0) {
    perror("Failed to set interface up");
    close(ctl_sock);
    close(sock);
    return -1;
  }

  printf("Successfully created and configured network interfaces\n");
  close(ctl_sock);
  close(sock);
  return 0;
}

int move_to_netns(pid_t pid, const char *interface) {
  char cmd[200];
  char pid_str[16];

  printf("\nPreparing to move interface %s to PID %d\n", interface, pid);

  // First verify the interface exists
  snprintf(cmd, sizeof(cmd), "ip link show %s", interface);
  printf("\nVerifying interface existence:\n");
  if (system(cmd) != 0) {
    printf("Interface %s not found before namespace setup!\n", interface);
    return -1;
  }

  // Ensure the netns directory exists with proper permissions
  printf("\nSetting up network namespace directory...\n");
  system("mkdir -p /var/run/netns");
  system("mount -t tmpfs tmpfs /var/run/netns");

  // Create a handle for the network namespace
  snprintf(pid_str, sizeof(pid_str), "%d", pid);

  // Verify the process exists
  snprintf(cmd, sizeof(cmd), "test -d /proc/%s", pid_str);
  if (system(cmd) != 0) {
    printf("Process %s does not exist!\n", pid_str);
    return -1;
  }

  // Verify process network namespace exists
  snprintf(cmd, sizeof(cmd), "test -e /proc/%s/ns/net", pid_str);
  if (system(cmd) != 0) {
    printf("Network namespace for process %s not found!\n", pid_str);
    return -1;
  }

  printf("Creating network namespace handle for PID %s...\n", pid_str);

  // Create and mount the network namespace
  snprintf(cmd, sizeof(cmd),
           "touch /var/run/netns/%s && "
           "mount --bind /proc/%s/ns/net /var/run/netns/%s",
           pid_str, pid_str, pid_str);
  printf("Setting up namespace with: %s\n", cmd);
  if (system(cmd) != 0) {
    printf("Failed to set up network namespace!\n");
    return -1;
  }

  // Verify namespace is properly set up
  snprintf(cmd, sizeof(cmd), "ls -l /var/run/netns/%s", pid_str);
  printf("\nVerifying namespace setup:\n");
  system(cmd);

  // Move interface to namespace
  snprintf(cmd, sizeof(cmd), "ip link set %s netns %s", interface, pid_str);
  printf("\nMoving interface with command: %s\n", cmd);
  int result = system(cmd);

  if (result != 0) {
    printf("Failed to move interface. Current interfaces:\n");
    system("ip link show");
    return -1;
  }

  return 0;
}

void setup_container_network(const struct netconfig *net) {
  char cmd[200];

  // Set container interface up
  snprintf(cmd, sizeof(cmd), "ip link set %s up", net->container_veth);
  system(cmd);

  // Assign IP address to container interface
  snprintf(cmd, sizeof(cmd), "ip addr add %s/%s dev %s", net->container_ip,
           net->netmask, net->container_veth);
  system(cmd);

  // Add default route
  snprintf(cmd, sizeof(cmd), "ip route add default via %s", net->host_ip);
  system(cmd);
}

void setup_host_network(const struct netconfig *net) {
  char cmd[200];

  // Set host interface up
  snprintf(cmd, sizeof(cmd), "ip link set %s up", net->host_veth);
  printf("Setting up host interface: %s\n", cmd);
  system(cmd);

  // Assign IP address to host interface
  snprintf(cmd, sizeof(cmd), "ip addr add %s/%s dev %s", net->host_ip,
           net->netmask, net->host_veth);
  printf("Assigning IP to host interface: %s\n", cmd);
  system(cmd);

  // Note: Skipping IP forwarding and iptables in Docker environment
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
  if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == -1) {
    handle_error("mount private");
  }

  const char *base_dirs[] = {"bin", "etc", "lib", "usr", "sbin", NULL};

  printf("Copying root filesystem...\n");
  struct stat st;

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

  const char *dirs[] = {CONTAINER_ROOT "/proc", CONTAINER_ROOT "/sys",
                        CONTAINER_ROOT "/dev", CONTAINER_ROOT "/tmp", NULL};

  for (const char **dir = dirs; *dir != NULL; dir++) {
    if (mkdir(*dir, 0755) && errno != EEXIST) {
      handle_error("mkdir");
    }
  }

  // Mount proc filesystem
  if (mount("proc", CONTAINER_ROOT "/proc", "proc", 0, NULL) == -1) {
    handle_error("mount proc");
  }

  // Mount sysfs
  if (mount("sysfs", CONTAINER_ROOT "/sys", "sysfs", 0, NULL) == -1) {
    handle_error("mount sysfs");
  }

  // Mount devtmpfs
  if (mount("devtmpfs", CONTAINER_ROOT "/dev", "devtmpfs", 0, NULL) == -1) {
    handle_error("mount devtmpfs");
  }
}

void cleanup_filesystem(void) {
  umount(CONTAINER_ROOT "/dev");
  umount(CONTAINER_ROOT "/sys");
  umount(CONTAINER_ROOT "/proc");

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

  printf("Setting up container network...\n");
  setup_container_network(&args->net);

  printf("Changing root...\n");
  if (chroot(CONTAINER_ROOT) == -1) {
    handle_error("chroot");
  }

  if (chdir("/") == -1) {
    handle_error("chdir");
  }

  // Setup loopback interface
  system("ip link set lo up");

  char **argv = args->argv;
  if (execvp(argv[3], &argv[3]) == -1) {
    handle_error("execvp");
  }

  return 0;
}

int main(int argc, char *argv[]) {
  setbuf(stdout, NULL);
  setbuf(stderr, NULL);

  // Initialize random number generator
  srand(time(NULL));

  if (argc < 4) {
    fprintf(stderr, "Usage: %s run <image> <command> [args...]\n", argv[0]);
    exit(1);
  }

  if (strcmp(argv[1], "run") == 0) {
    if (mkdir(CONTAINER_ROOT, 0755) && errno != EEXIST) {
      handle_error("mkdir");
    }

    // Generate random interface names to avoid conflicts
    char host_veth[IFNAMSIZ];
    char container_veth[IFNAMSIZ];
    snprintf(host_veth, IFNAMSIZ, "veth%d", rand() % 10000);
    snprintf(container_veth, IFNAMSIZ, "veth%d", rand() % 10000);

    struct child_args args = {.argv = argv,
                              .net = {.container_ip = "172.16.0.2",
                                      .host_ip = "172.16.0.1",
                                      .netmask = "24"}};

    // Copy the generated interface names
    strncpy(args.net.host_veth, host_veth, IFNAMSIZ);
    strncpy(args.net.container_veth, container_veth, IFNAMSIZ);

    printf("Cleaning up existing network interfaces...\n");
    cleanup_network(args.net.host_veth, args.net.container_veth);

    printf("Creating network interfaces...\n");
    if (create_veth_pair(args.net.host_veth, args.net.container_veth) != 0) {
      fprintf(stderr, "Failed to create veth pair\n");
      exit(1);
    }

    char *stack = malloc(STACK_SIZE);
    if (stack == NULL) {
      handle_error("malloc");
    }

    int clone_flags =
        CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNET;

    pid_t pid =
        clone(child_function, stack + STACK_SIZE, clone_flags | SIGCHLD, &args);

    if (pid == -1) {
      handle_error("clone");
    }

    args.child_pid = pid;

    printf("Setting up host network...\n");
    setup_host_network(&args.net);

    printf("Moving interface to container namespace...\n");
    if (move_to_netns(pid, args.net.container_veth) != 0) {
      fprintf(stderr, "Failed to move interface to container namespace\n");
    }

    int status;
    if (waitpid(pid, &status, 0) == -1) {
      handle_error("waitpid");
    }

    cleanup_filesystem();
    free(stack);

    if (WIFEXITED(status)) {
      printf("Container exited with status %d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
      printf("Container killed by signal %d\n", WTERMSIG(status));
    }
  } else {
    fprintf(stderr, "Unknown command: %s\n", argv[1]);
    exit(1);
  }

  return 0;
}