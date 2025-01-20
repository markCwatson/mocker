#include "networking.h"
#include "common.h"
#include "logging.h"

#define VETH_HOST "veth0"
#define VETH_CONTAINER "ceth0"
#define HOST_IP "172.18.0.1"
#define CONTAINER_IP "172.18.0.2"
#define NETMASK "16"

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
    int ret = -1;

    LOG("Setting up container networking...\n");

    // Create veth pair
    snprintf(cmd, sizeof(cmd),
             "ip link add %s type veth peer name %s",
             VETH_HOST, VETH_CONTAINER);
    if (system(cmd) != 0)
    {
        LOG("Failed to create veth pair\n");
        goto cleanup;
    }

    // Move container end to child's network namespace
    snprintf(cmd, sizeof(cmd),
             "ip link set %s netns %d",
             VETH_CONTAINER, child_pid);
    if (system(cmd) != 0)
    {
        LOG("Failed to move interface to container namespace\n");
        goto cleanup;
    }

    // Setup host end
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

    // Setup container end
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

    LOG("Network setup completed successfully\n");
    return 0;

cleanup:
    LOG("Network setup failed, cleaning up...\n");
    cleanup_networking();
    return ret;
}