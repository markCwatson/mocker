#include "libmnl.h"
#include "../common.h"
#include "../logging.h"

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <libmnl/libmnl.h>
#include <net/if.h>
#include <linux/if.h>
#include <linux/if_addr.h>
#include <arpa/inet.h>

// \todo: make the log messages more meaningful. Too many duplicate messages.

// fallback (compiler is complaining about missing definitions)
#ifndef IFLA_VETH_INFO_PEER
#define IFLA_VETH_INFO_PEER 1
#endif

// This callback is invoked for each netlink message in the response.
// We check for netlink errors or an ACK.
static int netlink_response_cb(const struct nlmsghdr *nlh, void *data)
{
    (void)data;

    // Possible message types: NLMSG_ERROR, NLMSG_DONE, etc.
    switch (nlh->nlmsg_type)
    {
    case NLMSG_ERROR:
    {
        // netlink error
        struct nlmsgerr *err = (struct nlmsgerr *)mnl_nlmsg_get_payload(nlh);
        if (err->error)
        {
            // Negative value in err->error
            errno = -err->error;
            LOG("[LIBMNL] Netlink reported an error: %s\n", strerror(errno));
            return MNL_CB_ERROR;
        }
        // If err->error == 0, that’s a success ACK
        return MNL_CB_OK;
    }
    case NLMSG_DONE:
        // Terminate the netlink processing loop.
        return MNL_CB_STOP;
    default:
        // Just continue processing
        break;
    }

    LOG("[LIBMNL] netlink_response_cb -> MNL_CB_OK\n");
    return MNL_CB_OK;
}

static void construct_netlink_msg_header(struct nlmsghdr *nlh, uint16_t type, uint16_t flags, uint32_t seq)
{
    nlh->nlmsg_type = type;
    nlh->nlmsg_flags = flags;
    nlh->nlmsg_seq = seq;
}

static void build_set_ip_msg(struct veth_config_s *config, const char *ip, const int prefix_len, char *buf)
{
    struct ifaddrmsg *ifa;
    struct in_addr in_addr;

    LOG("[NET] Setting IP address %s/%d on interface %s\n", ip, prefix_len, config->host);
    if (inet_pton(AF_INET, ip, &in_addr) != 1)
    {
        LOG("[NET] Error: Invalid IP address: %s\n", ip);
        return;
    }

    // Construct the netlink message
    config->nlh = mnl_nlmsg_put_header(buf);
    construct_netlink_msg_header(config->nlh, RTM_NEWADDR, NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE | NLM_F_ACK, config->seq);

    // Add the ifaddrmsg structure
    ifa = mnl_nlmsg_put_extra_header(config->nlh, sizeof(struct ifaddrmsg));
    ifa->ifa_family = AF_INET;                     // IPv4
    ifa->ifa_prefixlen = prefix_len;               // Set prefix length
    ifa->ifa_flags = 0;                            // No special flags
    ifa->ifa_scope = RT_SCOPE_UNIVERSE;            // Global scope
    ifa->ifa_index = if_nametoindex(config->host); // Interface index

    // Add the IFA_LOCAL attribute (Primary IPv4 address)
    mnl_attr_put(config->nlh, IFA_LOCAL, sizeof(struct in_addr), &in_addr);

    // Add the IFA_ADDRESS attribute (Peer or broadcast address, same as local)
    mnl_attr_put(config->nlh, IFA_ADDRESS, sizeof(struct in_addr), &in_addr);
}

static void build_newroute_msg(struct veth_config_s *config, const char *gateway_ip, char *buf)
{
    config->nlh = mnl_nlmsg_put_header(buf);
    construct_netlink_msg_header(config->nlh, RTM_NEWROUTE, NLM_F_REQUEST | NLM_F_CREATE | NLM_F_ACK, config->seq);

    struct rtmsg *rtm;
    rtm = mnl_nlmsg_put_extra_header(config->nlh, sizeof(struct rtmsg));
    rtm->rtm_family = AF_INET;          // IPv4
    rtm->rtm_dst_len = 0;               // Default route
    rtm->rtm_src_len = 0;               // No source filter
    rtm->rtm_table = RT_TABLE_MAIN;     // Main routing table
    rtm->rtm_protocol = RTPROT_STATIC;  // Static route
    rtm->rtm_scope = RT_SCOPE_UNIVERSE; // Global scope
    rtm->rtm_type = RTN_UNICAST;        // Unicast route

    // Add the gateway attribute (RTA_GATEWAY)
    struct in_addr gateway;
    if (inet_pton(AF_INET, gateway_ip, &gateway) != 1)
    {
        LOG("[NET] build_newroute_msg: Error: Invalid gateway IP address\n");
        mnl_socket_close(config->nl);
        return;
    }

    mnl_attr_put_u32(config->nlh, RTA_GATEWAY, gateway.s_addr);

    // Add the output interface (RTA_OIF)
    uint32_t ifindex = if_nametoindex(config->cont);
    if (ifindex == 0)
    {
        LOG("[NET] build_newroute_msg: Error: Failed to get index for interface %s\n", config->cont);
        mnl_socket_close(config->nl);
        return;
    }

    mnl_attr_put_u32(config->nlh, RTA_OIF, ifindex);
}

static void build_link_up_msg(struct veth_config_s *config, char *buf)
{
    config->nlh = mnl_nlmsg_put_header(buf);
    construct_netlink_msg_header(config->nlh, RTM_NEWLINK, NLM_F_REQUEST | NLM_F_ACK, config->seq);

    // Add ifinfomsg structure
    config->ifi = mnl_nlmsg_put_extra_header(config->nlh, sizeof(struct ifinfomsg));
    config->ifi->ifi_family = AF_UNSPEC;
    config->ifi->ifi_index = if_nametoindex(config->host);
    config->ifi->ifi_flags = IFF_UP;
    config->ifi->ifi_change = IFF_UP;

    // Add the interface name attribute
    mnl_attr_put_strz(config->nlh, IFLA_IFNAME, config->host);
}

static void build_setlink_msg(struct veth_config_s *config, char *buf)
{
    config->nlh = mnl_nlmsg_put_header(buf);
    construct_netlink_msg_header(config->nlh, RTM_SETLINK, NLM_F_REQUEST | NLM_F_ACK, config->seq);

    config->ifi = mnl_nlmsg_put_extra_header(config->nlh, sizeof(struct ifinfomsg));
    config->ifi->ifi_family = AF_UNSPEC;

    // Add the interface name
    mnl_attr_put_strz(config->nlh, IFLA_IFNAME, config->cont);

    // Add the target namespace PID
    mnl_attr_put_u32(config->nlh, IFLA_NET_NS_PID, config->child_pid);
}

static void build_netlink_msg(struct veth_config_s *config, char *buf)
{
    config->nlh = mnl_nlmsg_put_header(buf);
    construct_netlink_msg_header(config->nlh, RTM_NEWLINK, NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL, config->seq);

    // Outer ifinfomsg for the host interface
    // Note: u have to invoke mnl_nlmsg_put_header() before you call this function.
    config->ifi = mnl_nlmsg_put_extra_header(config->nlh, sizeof(struct ifinfomsg));
    config->ifi->ifi_family = AF_UNSPEC;

    // IFLA_IFNAME = host
    mnl_attr_put_strz(config->nlh, IFLA_IFNAME, config->host);

    // IFLA_LINKINFO
    LOG("[LIBMNL] Nesting IFLA_LINKINFO\n");
    struct nlattr *linkinfo = mnl_attr_nest_start(config->nlh, IFLA_LINKINFO);
    {
        mnl_attr_put_strz(config->nlh, IFLA_INFO_KIND, "veth");

        // IFLA_INFO_DATA
        LOG("[LIBMNL] Nesting IFLA_INFO_DATA\n");
        struct nlattr *infodata = mnl_attr_nest_start(config->nlh, IFLA_INFO_DATA);
        {
            // The kernel expects IFLA_VETH_INFO_PEER with a peer ifinfomsg
            LOG("[LIBMNL] Nesting IFLA_VETH_INFO_PEER\n");
            struct nlattr *peerinfo = mnl_attr_nest_start(config->nlh, IFLA_VETH_INFO_PEER);
            {
                struct ifinfomsg peer_ifi = {
                    .ifi_family = AF_UNSPEC,
                };

                // Put the peer ifinfomsg directly into the message
                LOG("[LIBMNL] Adding peer ifinfomsg\n");
                size_t sz = NLMSG_ALIGN(sizeof(peer_ifi));
                memcpy(mnl_nlmsg_get_payload_tail(config->nlh), &peer_ifi, sizeof(peer_ifi));
                config->nlh->nlmsg_len += sz;

                // Then the peer’s name
                mnl_attr_put_strz(config->nlh, IFLA_IFNAME, config->cont);
                LOG("[LIBMNL] Peer name: %s\n", config->cont);
            }
            mnl_attr_nest_end(config->nlh, peerinfo);
        }
        mnl_attr_nest_end(config->nlh, infodata);
    }
    mnl_attr_nest_end(config->nlh, linkinfo);
}

static int receive_netlink_responses(struct veth_config_s *config)
{
    char buf[MNL_SOCKET_BUFFER_SIZE];
    int ret;

    while (true)
    {
        ret = mnl_socket_recvfrom(config->nl, buf, sizeof(buf));
        if (ret == -1)
        {
            if (errno == EAGAIN)
            {
                LOG("[LIBMNL] mnl_socket_recvfrom: errno == EAGAIN\n");
                break;
            }

            LOG("[LIBMNL] mnl_socket_recvfrom: errno != EAGAIN\n");
            mnl_socket_close(config->nl);
            return EXIT_FAILURE;
        }

        if (ret == 0)
        {
            LOG("[LIBMNL] mnl_socket_recvfrom: EOF\n");
            break;
        }

        // Let libmnl parse the messages; netlink_response_cb will handle errors
        ret = mnl_cb_run(buf, ret, 0, config->seq, netlink_response_cb, NULL);
        if (ret <= 0)
        {
            LOG("[LIBMNL] mnl_cb_run: MNL_CB_ERROR or MNL_CB_STOP\n");
            break;
        }
    }

    return EXIT_SUCCESS;
}

static int open_and_bind_netlink_socket(struct mnl_socket **nl)
{
    // Open Netlink socket
    LOG("[LIBMNL] Opening Netlink socket\n");
    *nl = mnl_socket_open(NETLINK_ROUTE);
    if (!*nl)
    {
        LOG("[LIBMNL] mnl_socket_open");
        goto error;
    }

    // Bind to netlink
    LOG("[LIBMNL] Binding to Netlink\n");
    if (mnl_socket_bind(*nl, 0, MNL_SOCKET_AUTOPID) < 0)
    {
        LOG("[LIBMNL] mnl_socket_bind\n");
        goto error_with_socket;
    }

    return EXIT_SUCCESS;

error_with_socket:
    mnl_socket_close(*nl);
error:
    return EXIT_FAILURE;
}

// \todo: not sure how to use libmnl to do this....
int setup_nat_rules(struct veth_config_s *veth_config, const char *container_network)
{
    char cmd[256];

    // Clear any existing NAT rules for our network
    snprintf(cmd, sizeof(cmd),
             "iptables -t nat -D POSTROUTING -s %s ! -o %s -j MASQUERADE 2>/dev/null",
             container_network, veth_config->host);
    system(cmd);

    // Add NAT rule
    snprintf(cmd, sizeof(cmd),
             "iptables -t nat -A POSTROUTING -s %s ! -o %s -j MASQUERADE",
             container_network, veth_config->host);
    if (system(cmd) != 0)
    {
        LOG("[NET] Failed to set up NAT rules\n");
        return -1;
    }

    return 0;
}

int set_interface_ip(struct veth_config_s *veth_config, const char *iface, const char *ip, const int prefix_len)
{
    char buf[MNL_SOCKET_BUFFER_SIZE];
    veth_config->host = iface;
    veth_config->seq = (uint32_t)time(NULL);

    if (open_and_bind_netlink_socket(&veth_config->nl) != EXIT_SUCCESS)
    {
        LOG("[NET] Error: Failed to open netlink socket\n");
        return -1;
    }

    // Build and send the netlink message
    build_set_ip_msg(veth_config, ip, prefix_len, buf);
    if (mnl_socket_sendto(veth_config->nl, veth_config->nlh, veth_config->nlh->nlmsg_len) < 0)
    {
        LOG("[NET] Error: Failed to send netlink message\n");
        mnl_socket_close(veth_config->nl);
        return EXIT_FAILURE;
    }

    // Process responses
    if (receive_netlink_responses(veth_config) != EXIT_SUCCESS)
    {
        LOG("[NET] Error: Failed to set interface IP\n");
        mnl_socket_close(veth_config->nl);
        return EXIT_FAILURE;
    }

    mnl_socket_close(veth_config->nl);
    return EXIT_SUCCESS;
}

int set_default_route(struct veth_config_s *veth_config, const char *gateway_ip)
{
    char buf[MNL_SOCKET_BUFFER_SIZE];
    veth_config->seq = (uint32_t)time(NULL);

    if (open_and_bind_netlink_socket(&veth_config->nl) != EXIT_SUCCESS)
    {
        LOG("[NET] Error: Failed to open netlink socket\n");
        return EXIT_FAILURE;
    }

    build_newroute_msg(veth_config, gateway_ip, buf);
    if (mnl_socket_sendto(veth_config->nl, veth_config->nlh, veth_config->nlh->nlmsg_len) < 0)
    {
        LOG("[NET] Error: Failed to send netlink message\n");
        mnl_socket_close(veth_config->nl);
        return EXIT_FAILURE;
    }

    if (receive_netlink_responses(veth_config) != EXIT_SUCCESS)
    {
        LOG("[NET] Error: Failed to set default route\n");
        mnl_socket_close(veth_config->nl);
        return EXIT_FAILURE;
    }

    mnl_socket_close(veth_config->nl);
    return EXIT_SUCCESS;
}

int set_interface_up(struct veth_config_s *veth_config, const char *iface)
{
    char buf[MNL_SOCKET_BUFFER_SIZE];
    veth_config->host = iface;
    veth_config->seq = (uint32_t)time(NULL);

    if (open_and_bind_netlink_socket(&veth_config->nl) != EXIT_SUCCESS)
    {
        LOG("[NET] Error: Failed to open netlink socket\n");
        return -1;
    }

    // Build and send the netlink message
    build_link_up_msg(veth_config, buf);
    if (mnl_socket_sendto(veth_config->nl, veth_config->nlh, veth_config->nlh->nlmsg_len) < 0)
    {
        LOG("[NET] Error: Failed to send netlink message\n");
        mnl_socket_close(veth_config->nl);
        return EXIT_FAILURE;
    }

    // Process responses
    if (receive_netlink_responses(veth_config) != EXIT_SUCCESS)
    {
        LOG("[NET] Error: Failed to set interface up\n");
        mnl_socket_close(veth_config->nl);
        return EXIT_FAILURE;
    }

    mnl_socket_close(veth_config->nl);
    return EXIT_SUCCESS;
}

int move_veth_to_ns(struct veth_config_s *veth_config)
{
    char buf[MNL_SOCKET_BUFFER_SIZE];
    veth_config->seq = (uint32_t)time(NULL);

    if (open_and_bind_netlink_socket(&veth_config->nl) != EXIT_SUCCESS)
    {
        LOG("[LIBMNL] Error: open_and_bind_netlink_socket\n");
        return EXIT_FAILURE;
    }

    // Build the Netlink message
    LOG("[LIBMNL] Building RTM_SETLINK message\n");
    build_setlink_msg(veth_config, buf);

    // Send the message
    LOG("[LIBMNL] Sending Netlink message\n");
    if (mnl_socket_sendto(veth_config->nl, veth_config->nlh, veth_config->nlh->nlmsg_len) < 0)
    {
        LOG("[LIBMNL] Error: mnl_socket_sendto\n");
        goto error;
    }

    // Receive and parse all responses in a loop
    LOG("[LIBMNL] Receiving Netlink responses\n");
    if (receive_netlink_responses(veth_config) != EXIT_SUCCESS)
    {
        LOG("[LIBMNL] Error: receive_netlink_responses\n");
        goto error;
    }

    LOG("[LIBMNL] Successfully moved interface to namespace\n");
    mnl_socket_close(veth_config->nl);
    return EXIT_SUCCESS;

error:
    mnl_socket_close(veth_config->nl);
    return EXIT_FAILURE;
}

int create_veth_pair(struct veth_config_s *veth_config)
{
    char buf[MNL_SOCKET_BUFFER_SIZE];
    veth_config->seq = (uint32_t)time(NULL);

    if (open_and_bind_netlink_socket(&veth_config->nl) != EXIT_SUCCESS)
    {
        LOG("[LIBMNL] Error: open_and_bind_netlink_socket\n");
        return EXIT_FAILURE;
    }

    // Build the Netlink message
    LOG("[LIBMNL] Building Netlink message\n");
    build_netlink_msg(veth_config, buf);

    // Send the message
    LOG("[LIBMNL] Sending Netlink message\n");
    if (mnl_socket_sendto(veth_config->nl, veth_config->nlh, veth_config->nlh->nlmsg_len) < 0)
    {
        LOG("[LIBMNL] Error: mnl_socket_sendto\n");
        goto error;
    }

    // Receive and parse all responses in a loop
    LOG("[LIBMNL] Receiving Netlink responses\n");
    if (receive_netlink_responses(veth_config) != EXIT_SUCCESS)
    {
        LOG("[LIBMNL] Error: receive_netlink_responses\n");
        goto error;
    }

    LOG("[LIBMNL] veth pair created successfully\n");
    mnl_socket_close(veth_config->nl);
    return EXIT_SUCCESS;

error:
    mnl_socket_close(veth_config->nl);
    return EXIT_FAILURE;
}
