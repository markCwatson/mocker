#define _GNU_SOURCE // note: i needed this for setns ?? see `man 2 setns`
#include "networking.h"
#include "libmnl.h"
#include "../logging.h"

#define VETH_HOST "veth0"
#define VETH_CONTAINER "ceth0"
#define HOST_IP "172.18.0.1"
#define CONTAINER_IP "172.18.0.2"
#define NETMASK "16"
#define CONTAINER_NETWORK "172.18.0.0/16"

static int switch_to_container_ns(struct veth_config_s *veth_config)
{
    char ns_path[256];
    snprintf(ns_path, sizeof(ns_path), "/proc/%d/ns/%s", veth_config->child_pid, veth_config->child_namespace);

    int fd = open(ns_path, O_RDONLY);
    if (fd == -1)
    {
        perror("open namespace");
        LOG("[LIBMNL] switch_to_container_ns: Error: open namespace\n");
        return EXIT_FAILURE;
    }

    if (setns(fd, 0) == -1)
    {
        perror("setns");
        LOG("[LIBMNL] switch_to_container_ns: Error: setns\n");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static int save_current_namespace(const char *namespace, int *ns_fd)
{
    char ns_path[256];
    snprintf(ns_path, sizeof(ns_path), "/proc/self/ns/%s", namespace);

    *ns_fd = open(ns_path, O_RDONLY);
    if (*ns_fd == -1)
    {
        perror("open host namespace");
        LOG("[NET] Failed to open self namespace\n");
        return -1;
    }

    return 0;
}

static int restore_namespace(int ns_fd)
{
    if (setns(ns_fd, 0) == -1)
    {
        perror("setns");
        LOG("[NET] Failed to restore namespace\n");
        close(ns_fd);
        return -1;
    }

    close(ns_fd);
    return 0;
}

static int setup_dns(void)
{
    char etc_path[256];
    char src_file[] = "/etc/resolv.conf";
    char dst_file[256];
    int fd_src = -1;
    int fd_dst = -1;

    // i.e. mkdir -p CONTAINER_ROOT/etc
    snprintf(etc_path, sizeof(etc_path), "%s/etc", CONTAINER_ROOT);
    if (mkdir(etc_path, 0755) == -1 && errno != EEXIST)
    {
        LOG("[NET] Failed to create directory %s: %s\n",
            etc_path, strerror(errno));
        return -1;
    }

    // open /etc/resolv.conf to read from
    fd_src = open(src_file, O_RDONLY);
    if (fd_src < 0)
    {
        LOG("[NET] Failed to open source %s: %s\n",
            src_file, strerror(errno));
        return -1;
    }

    // open CONTAINER_ROOT/etc/resolv.conf to write to
    snprintf(dst_file, sizeof(dst_file), "%s/etc/resolv.conf", CONTAINER_ROOT);
    fd_dst = open(dst_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_dst < 0)
    {
        LOG("[NET] Failed to open destination %s: %s\n",
            dst_file, strerror(errno));
        close(fd_src);
        return -1;
    }

    // cp /etc/resolv.conf CONTAINER_ROOT/etc/resolv.conf
    {
        char buffer[4096];
        ssize_t bytes_read;

        while ((bytes_read = read(fd_src, buffer, sizeof(buffer))) > 0)
        {
            ssize_t bytes_written = 0;
            char *write_ptr = buffer;

            while (bytes_written < bytes_read)
            {
                ssize_t w = write(fd_dst, write_ptr + bytes_written,
                                  bytes_read - bytes_written);
                if (w < 0)
                {
                    LOG("[NET] Write failed: %s\n", strerror(errno));
                    close(fd_src);
                    close(fd_dst);
                    return -1;
                }
                bytes_written += w;
            }
        }

        if (bytes_read < 0)
        {
            LOG("[NET] Read failed: %s\n", strerror(errno));
            close(fd_src);
            close(fd_dst);
            return -1;
        }
    }

    close(fd_src);
    close(fd_dst);

    LOG("[NET] DNS configuration successfully copied.\n");
    return 0;
}

static int enable_ip_forwarding(void)
{
    const char *forwarding_file = "/proc/sys/net/ipv4/ip_forward";
    FILE *fp = fopen(forwarding_file, "w");
    if (!fp)
    {
        LOG("[NET] Failed to open %s\n", forwarding_file);
        return -1;
    }

    fprintf(fp, "1");
    fclose(fp);

    return 0;
}

static int setup_nat_rules(void)
{
    char cmd[256];

    // Clear any existing NAT rules for our network
    snprintf(cmd, sizeof(cmd),
             "iptables -t nat -D POSTROUTING -s %s ! -o %s -j MASQUERADE 2>/dev/null",
             CONTAINER_NETWORK, VETH_HOST);
    system(cmd);

    // Add NAT rule
    snprintf(cmd, sizeof(cmd),
             "iptables -t nat -A POSTROUTING -s %s ! -o %s -j MASQUERADE",
             CONTAINER_NETWORK, VETH_HOST);
    if (system(cmd) != 0)
    {
        LOG("[NET] Failed to set up NAT rules\n");
        return -1;
    }

    return 0;
}

static void cleanup_nat_rules(void)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "iptables -t nat -D POSTROUTING -s %s ! -o %s -j MASQUERADE 2>/dev/null",
             CONTAINER_NETWORK, VETH_HOST);
    system(cmd);
}

void cleanup_networking(void)
{
    char cmd[256];
    LOG("[NET] Cleaning up network interfaces...\n");

    // Delete veth pair (deleting one end automatically removes the peer)
    snprintf(cmd, sizeof(cmd), "ip link delete %s 2>/dev/null", VETH_HOST);
    system(cmd);
}

int setup_networking(pid_t child_pid)
{
    int host_ns_fd;

    struct veth_config_s veth_config = {
        .child_pid = child_pid,
        .child_namespace = "net",
        .host = VETH_HOST,
        .cont = VETH_CONTAINER,
        .nl = NULL,
        .nlh = NULL,
        .ifi = NULL,
        .seq = 0,
    };

    LOG("[NET] Setting up container networking...\n");

    // Save the current (host) namespace
    if (save_current_namespace("net", &host_ns_fd) != 0)
    {
        LOG("[NET] Failed to save host namespace\n");
        goto cleanup;
    }

    // i.e. mkdir -p CONTAINER_ROOT/etc
    //      && cp /etc/resolv.conf CONTAINER_ROOT/etc/resolv.conf
    if (setup_dns() != 0)
    {
        LOG("[NET] Failed to setup DNS\n");
        goto cleanup;
    }

    // create veth pair
    // i.e. ip link add VETH_HOST type veth peer name VETH_CONTAINER
    if (create_veth_pair(&veth_config) != 0)
    {
        LOG("[NET] Failed to create veth pair\n");
        goto cleanup;
    }

    // move container end to child's network namespace
    // i.e. ip link set VETH_CONTAINER netns child_pid
    if (move_veth_to_ns(&veth_config) != 0)
    {
        LOG("[NET] Failed to move interface to container namespace\n");
        goto cleanup;
    }

    // setup host end
    // i.e. ip link set VETH_HOST up"
    if (set_interface_up(&veth_config, VETH_HOST) != 0)
    {
        LOG("[NET] Failed to set host interface up\n");
        goto cleanup;
    }

    // setup host ip address
    // i.e. ip addr add HOST_IP/NETMASK dev VETH_HOST
    // \todo: change NETMASK to 16 (int instead of string) and pass in here
    if (set_interface_ip(&veth_config, VETH_HOST, HOST_IP, 16) != 0)
    {
        LOG("[NET] Failed to set host IP\n");
        goto cleanup;
    }

    // setup container end
    // i.e. nsenter -t child_pid
    if (switch_to_container_ns(&veth_config) != 0)
    {
        LOG("[NET] Failed to switch to container namespace\n");
        goto cleanup;
    }

    if (set_interface_up(&veth_config, "lo") != 0)
    {
        LOG("[NET] Failed to set up loopback interface in container\n");
        goto cleanup;
    }

    if (set_interface_up(&veth_config, VETH_CONTAINER) != 0)
    {
        LOG("[NET] Failed to set up container interface\n");
        goto cleanup;
    }

    if (set_interface_ip(&veth_config, VETH_CONTAINER, CONTAINER_IP, 16) != 0)
    {
        LOG("[NET] Failed to set container IP\n");
        goto cleanup;
    }

    // set default route in container (still in container's namespace)
    // i.e. ip route add default via HOST_IP
    if (set_default_route(&veth_config, HOST_IP) != 0)
    {
        LOG("[NET] Failed to switch to container namespace\n");
        goto cleanup;
    }

    if (restore_namespace(host_ns_fd) != 0)
    {
        LOG("[NET] Failed to restore host namespace\n");
        goto cleanup;
    }

    // \todo: convert the rest of this to use libmnl ....

    if (enable_ip_forwarding() != 0)
    {
        LOG("[NET] Failed to enable IP forwarding\n");
        goto cleanup;
    }

    if (setup_nat_rules() != 0)
    {
        LOG("[NET] Failed to setup NAT\n");
        goto cleanup;
    }

    LOG("[NET] Network setup completed successfully with NAT\n");
    return 0;

cleanup:
    LOG("[NET] Network setup failed, cleaning up...\n");
    cleanup_networking();
    cleanup_nat_rules();
    return -1;
}