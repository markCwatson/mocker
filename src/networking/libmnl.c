#include "libmnl.h"
#include "../common.h"
#include "../logging.h"

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <libmnl/libmnl.h>

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
                LOG("[LIBMNL] Peer name: %s\n", cont);
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

            LOG("[LIBMNL] mnl_socket_recvfrom: errno != EAGAIN\n")
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
        LOG("[LIBMNL] mnl_socket_bind");
        goto error_with_socket;
    }

    return EXIT_SUCCESS;

error_with_socket:
    mnl_socket_close(*nl);
error:
    return EXIT_FAILURE;
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
        LOG("[LIBMNL] Error: mnl_socket_sendto");
        goto error;
    }

    // Receive and parse all responses in a loop
    LOG("[LIBMNL] Receiving Netlink responses\n");
    if (receive_netlink_responses(veth_config) != EXIT_SUCCESS)
    {
        LOG("[LIBMNL] Error: receive_netlink_responses\n");
        goto error;
    }

    LOG("[LIBMNL] Successfully moved interface to namespace");
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
        LOG("[LIBMNL] Error: mnl_socket_sendto");
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
