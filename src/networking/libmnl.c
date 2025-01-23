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

int create_veth_pair(const char *host, const char *cont)
{
    struct mnl_socket *nl;
    char buf[MNL_SOCKET_BUFFER_SIZE];
    struct nlmsghdr *nlh;
    struct ifinfomsg *ifi;
    uint32_t seq;
    int ret;

    // Open Netlink socket
    LOG("[LIBMNL] Opening Netlink socket\n");
    nl = mnl_socket_open(NETLINK_ROUTE);
    if (!nl)
    {
        LOG("[LIBMNL] mnl_socket_open");
        return EXIT_FAILURE;
    }

    // Bind to netlink
    LOG("[LIBMNL] Binding to Netlink\n");
    if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0)
    {
        LOG("[LIBMNL] mnl_socket_bind");
        mnl_socket_close(nl);
        return EXIT_FAILURE;
    }

    // Build the Netlink message
    LOG("[LIBMNL] Building Netlink message\n");
    nlh = mnl_nlmsg_put_header(buf);
    nlh->nlmsg_type = RTM_NEWLINK;
    // We include NLM_F_ACK so the kernel sends us an ACK or error
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
    seq = (uint32_t)time(NULL);
    nlh->nlmsg_seq = seq;

    // Outer ifinfomsg for the "veth0" interface
    ifi = mnl_nlmsg_put_extra_header(nlh, sizeof(struct ifinfomsg));
    ifi->ifi_family = AF_UNSPEC;

    // IFLA_IFNAME = "veth0"
    mnl_attr_put_strz(nlh, IFLA_IFNAME, host);

    // IFLA_LINKINFO
    LOG("[LIBMNL] Nesting IFLA_LINKINFO\n");
    struct nlattr *linkinfo = mnl_attr_nest_start(nlh, IFLA_LINKINFO);
    {
        mnl_attr_put_strz(nlh, IFLA_INFO_KIND, "veth");

        // IFLA_INFO_DATA
        struct nlattr *infodata = mnl_attr_nest_start(nlh, IFLA_INFO_DATA);
        {
            // The kernel expects IFLA_VETH_INFO_PEER with a peer ifinfomsg
            struct nlattr *peerinfo = mnl_attr_nest_start(nlh, IFLA_VETH_INFO_PEER);
            {
                struct ifinfomsg peer_ifi = {
                    .ifi_family = AF_UNSPEC,
                };

                // Put the peer ifinfomsg directly into the message
                size_t sz = NLMSG_ALIGN(sizeof(peer_ifi));
                memcpy(mnl_nlmsg_get_payload_tail(nlh), &peer_ifi, sizeof(peer_ifi));
                nlh->nlmsg_len += sz;

                // Then the peer’s name
                mnl_attr_put_strz(nlh, IFLA_IFNAME, cont);
                LOG("[LIBMNL] Peer name: %s\n", cont);
            }
            mnl_attr_nest_end(nlh, peerinfo);
        }
        mnl_attr_nest_end(nlh, infodata);
    }
    mnl_attr_nest_end(nlh, linkinfo);

    // Send the message
    LOG("[LIBMNL] Sending Netlink message\n");
    if (mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0)
    {
        LOG("[LIBMNL] Error: mnl_socket_sendto");
        mnl_socket_close(nl);
        return EXIT_FAILURE;
    }

    // Receive and parse **all** responses in a loop
    LOG("[LIBMNL] Receiving Netlink responses\n");
    while (true)
    {
        ret = mnl_socket_recvfrom(nl, buf, sizeof(buf));
        if (ret == -1)
        {
            if (errno == EAGAIN)
            {
                LOG("[LIBMNL] EAGAIN\n");
                break;
            }
            LOG("[LIBMNL] Error: mnl_socket_recvfrom");
            mnl_socket_close(nl);
            return EXIT_FAILURE;
        }

        if (ret == 0)
        {
            LOG("[LIBMNL] EOF\n");
            break;
        }

        // Let libmnl parse the messages; netlink_response_cb will handle errors
        ret = mnl_cb_run(buf, ret, 0, seq, netlink_response_cb, NULL);
        if (ret <= 0)
        {
            // MNL_CB_ERROR or MNL_CB_STOP => we stop receiving
            break;
        }
    }

    // If we reached here without an error, the veth creation succeeded.
    LOG("[LIBMNL] veth pair created successfully\n");

    mnl_socket_close(nl);
    return EXIT_SUCCESS;
}
