#include "networking.h"
#include "../logging.h"
#include "libmnl.h"

#define VETH_HOST "veth0"
#define VETH_CONTAINER "ceth0"
#define HOST_IP "172.18.0.1"
#define CONTAINER_IP "172.18.0.2"
#define NETMASK "16"
#define CONTAINER_NETWORK "172.18.0.0/16"

int setup_dns(pid_t child_pid)
{
    char cmd[256];

    // Copy host's resolv.conf to container
    snprintf(cmd, sizeof(cmd),
             "mkdir -p %s/etc && cp /etc/resolv.conf %s/etc/resolv.conf",
             CONTAINER_ROOT, CONTAINER_ROOT);
    if (system(cmd) != 0)
    {
        LOG("Failed to setup DNS configuration\n");
        return -1;
    }

    return 0;
}

int enable_ip_forwarding(void)
{
    const char *forwarding_file = "/proc/sys/net/ipv4/ip_forward";
    FILE *fp = fopen(forwarding_file, "w");
    if (!fp)
    {
        LOG("Failed to open %s\n", forwarding_file);
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
        LOG("Failed to set up NAT rules\n");
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
    LOG("Cleaning up network interfaces...\n");

    // Delete veth pair (deleting one end automatically removes the peer)
    snprintf(cmd, sizeof(cmd), "ip link delete %s 2>/dev/null", VETH_HOST);
    system(cmd);
}

int setup_networking(pid_t child_pid)
{
    char cmd[256];

    LOG("Setting up container networking...\n");

    if (setup_dns(child_pid) != 0)
    {
        LOG("Failed to setup DNS\n");
        goto cleanup;
    }

    // create veth pair
    // i.e. ip link add VETH_HOST type veth peer name VETH_CONTAINER
    if (create_veth_pair(VETH_HOST, VETH_CONTAINER) != 0)
    {
        LOG("Failed to create veth pair\n");
        goto cleanup;
    }

    // move container end to child's network namespace
    snprintf(cmd, sizeof(cmd),
             "ip link set %s netns %d",
             VETH_CONTAINER, child_pid);
    if (system(cmd) != 0)
    {
        LOG("Failed to move interface to container namespace\n");
        goto cleanup;
    }

    // setup host end
    snprintf(cmd, sizeof(cmd), "ip link set %s up", VETH_HOST);
    if (system(cmd) != 0)
    {
        LOG("Failed to set host interface up\n");
        goto cleanup;
    }

    snprintf(cmd, sizeof(cmd),
             "ip addr add %s/%s dev %s",
             HOST_IP, NETMASK, VETH_HOST);
    if (system(cmd) != 0)
    {
        LOG("Failed to set host IP\n");
        goto cleanup;
    }

    // setup container end
    snprintf(cmd, sizeof(cmd),
             "nsenter -t %d -n ip link set lo up", child_pid);
    if (system(cmd) != 0)
    {
        LOG("Failed to set container loopback up\n");
        goto cleanup;
    }

    snprintf(cmd, sizeof(cmd),
             "nsenter -t %d -n ip link set %s up",
             child_pid, VETH_CONTAINER);
    if (system(cmd) != 0)
    {
        LOG("Failed to set container interface up\n");
        goto cleanup;
    }

    snprintf(cmd, sizeof(cmd),
             "nsenter -t %d -n ip addr add %s/%s dev %s",
             child_pid, CONTAINER_IP, NETMASK, VETH_CONTAINER);
    if (system(cmd) != 0)
    {
        LOG("Failed to set container IP\n");
        goto cleanup;
    }

    snprintf(cmd, sizeof(cmd),
             "nsenter -t %d -n ip route add default via %s",
             child_pid, HOST_IP);
    if (system(cmd) != 0)
    {
        LOG("Failed to set container default route\n");
        goto cleanup;
    }

    if (enable_ip_forwarding() != 0)
    {
        LOG("Failed to enable IP forwarding\n");
        goto cleanup;
    }

    if (setup_nat_rules() != 0)
    {
        LOG("Failed to setup NAT\n");
        goto cleanup;
    }

    LOG("Network setup completed successfully with NAT\n");
    return 0;

cleanup:
    LOG("Network setup failed, cleaning up...\n");
    cleanup_networking();
    cleanup_nat_rules();
    return -1;
}