#ifndef __LIBMNL__H__
#define __LIBMNL__H__

#include <sys/types.h> // for pid_t
#include <stdint.h>    // for uint32_t

// forward declarations to avoid including libmnl headers
struct mnl_socket;
struct nlmsghdr;
struct ifinfomsg;

struct veth_config_s
{
    const uint16_t child_pid;
    const char *child_namespace;
    const char *host;
    const char *cont;
    struct mnl_socket *nl;
    struct nlmsghdr *nlh;
    struct ifinfomsg *ifi;
    uint32_t seq;
};

int set_default_route(struct veth_config_s *veth_config, const char *gateway_ip);
int set_interface_ip(struct veth_config_s *veth_config, const char *iface, const char *ip, const int prefix_len);
int set_interface_up(struct veth_config_s *veth_config, const char *iface);
int move_veth_to_ns(struct veth_config_s *veth_config);
int create_veth_pair(struct veth_config_s *veth_config);

#endif