#include "networking.h"
#include "libmnl.h"
#include "../logging.h"

#define VETH_HOST "veth0"
#define VETH_CONTAINER "ceth0"
#define HOST_IP "172.18.0.1"
#define CONTAINER_IP "172.18.0.2"
#define NETMASK "16"
#define CONTAINER_NETWORK "172.18.0.0/16"

int setup_dns(void)
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

int enable_ip_forwarding(void)
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

int setup_nat_rules(void)
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

void cleanup_nat_rules(void)
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
    char cmd[256];

    struct veth_config_s veth_config = {
        .child_pid = child_pid,
        .host = VETH_HOST,
        .cont = VETH_CONTAINER,
        .nl = NULL,
        .nlh = NULL,
        .ifi = NULL,
        .seq = 0,
    };

    LOG("[NET] Setting up container networking...\n");

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

    // \todo: convert the rest of this to use libmnl ....

    // setup host end
    snprintf(cmd, sizeof(cmd), "ip link set %s up", VETH_HOST);
    if (system(cmd) != 0)
    {
        LOG("[NET] Failed to set host interface up\n");
        goto cleanup;
    }

    snprintf(cmd, sizeof(cmd),
             "ip addr add %s/%s dev %s",
             HOST_IP, NETMASK, VETH_HOST);
    if (system(cmd) != 0)
    {
        LOG("[NET] Failed to set host IP\n");
        goto cleanup;
    }

    // setup container end
    snprintf(cmd, sizeof(cmd),
             "nsenter -t %d -n ip link set lo up", child_pid);
    if (system(cmd) != 0)
    {
        LOG("[NET] Failed to set container loopback up\n");
        goto cleanup;
    }

    snprintf(cmd, sizeof(cmd),
             "nsenter -t %d -n ip link set %s up",
             child_pid, VETH_CONTAINER);
    if (system(cmd) != 0)
    {
        LOG("[NET] Failed to set container interface up\n");
        goto cleanup;
    }

    snprintf(cmd, sizeof(cmd),
             "nsenter -t %d -n ip addr add %s/%s dev %s",
             child_pid, CONTAINER_IP, NETMASK, VETH_CONTAINER);
    if (system(cmd) != 0)
    {
        LOG("[NET] Failed to set container IP\n");
        goto cleanup;
    }

    snprintf(cmd, sizeof(cmd),
             "nsenter -t %d -n ip route add default via %s",
             child_pid, HOST_IP);
    if (system(cmd) != 0)
    {
        LOG("[NET] Failed to set container default route\n");
        goto cleanup;
    }

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