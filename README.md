# _mocker_: a minimal container runtime in C

![Build Status](https://github.com/markCwatson/mocker/actions/workflows/build.yml/badge.svg)

This project implements a basic container runtime in C that demonstrates core container concepts like process isolation, networking and filesystem isolation, and mount namespaces.

[![Mocker demo](public/youtube.png)](https://www.youtube.com/watch?v=MNBXOb73fxs 'mocker demo')

## Features

- **Process Isolation**: Uses Linux namespaces (PID, Mount, UTS, IPC) to isolate processes. The `clone()` system call is used with the appropriate flags to create isolated child processes, ensuring separation from the host system.

- **Filesystem Isolation**: Uses an isolated filesystem environment by:
  - Creating necessary directories for the container.
  - Copying the [BusyBox](https://www.busybox.net/downloads/BusyBox.html) binary to serve as a minimal set of Unix utilities.
  - Mapping symbolic links for essential commands.
  - Using [chroot](https://man7.org/linux/man-pages/man2/chroot.2.html) to change the root filesystem, ensuring containerized processes operate within their own environment.

- **Mount Namespace**: Impements filesystem isolation by configuring essential mounts within the container's root filesystem. Special filesystems like `proc`, `sys`, and `tmpfs` are mounted using the [mount](https://man7.org/linux/man-pages/man2/mount.2.html) system call to provide necessary system information and temporary storage.

- **Cgroups for Resource Control**: Uses [cgroups (Control Groups)](https://man7.org/linux/man-pages/man7/cgroups.7.html) to manage and limit resource usage for containerized processes. This includes:
  - Setting memory limits using the `memory.max` file.
  - Restricting CPU time allocation with the `cpu.max` file.
  - Assigning processes to cgroups via the `cgroup.procs` file.
  This ensures each container stays within its allocated resources, like memory and CPU, while maintaining system stability.

- **Networking**: Implements network namespace isolation and virtual Ethernet (veth) pair creation to enable container-host communication. Networking features include:
  - Configuring IP addresses and routes for both the container and host.
  - Enabling NAT for internet access using `iptables`.
  - Using [libmnl](https://www.netfilter.org/projects/libmnl/doxygen/html/) for communication with [netlink sockets](https://man7.org/linux/man-pages/man7/netlink.7.html) to handle network setup programmatically.

- **Cleanup and Resource Management**: Ensures proper resource cleanup to maintain system integrity, including:
  - Unmounting special filesystems (`proc`, `sys`, `tmpfs`) using [umount2](https://man7.org/linux/man-pages/man2/umount.2.html).
  - Deleting temporary directories and virtual Ethernet interfaces.
  - Removing cgroups created for the container.

## Requirements

- Linux system with namespace support and root provelages
- See dependencies below

## Building

```shell
# Install required packages (on Debian/Ubuntu)
sudo apt-get update
sudo apt-get install -y build-essential gcc make libmnl-dev busybox-static

# Use make to compile/buld
make

# use make to run (will open shell in container)
make run
```

## Usage

![alt-text][1]

You should run `mocker` inside a VM running Linux (tested with Kali) and execute a limited set of commands inside the _mocker_ container.

The container runtime accepts commands in this format:

```shell
sudo ./mocker run <image-name> <command> [args...]
```

Note: The `image-name` argument is currently a placeholder as image handling is not implemented.

Examples:

```shell
# List files in container
sudo ./mocker run ubuntu:latest /bin/ls /

# Run a shell in container
sudo ./mocker run ubuntu:latest /bin/sh

# Echo a message
sudo ./mocker run ubuntu:latest /bin/echo "Hello from container"

# Check processes
sudo ./mocker run ubuntu:latest /bin/ps

# Test networking
sudo ./mocker run ubuntu:latest /bin/sh
ip link ls                  # should show lo and ceth0
ip addr show dev ceth0      # should show IP address for veth in container
ip route show               # should show default route
ping -c 3 google.com        # should ping google (proves internet connectivity and DNS config)
# and from the host machine (in another terminal)
sudo iptables -t nat -L POSTROUTING -n # should show MASQUERADE rule
ip addr show dev veth0      # should show IP address for veth on host side
ip link ls                  # should show veth0 on host
sudo tcpdump -i veth0       # keep this open and run the ping in container and watch traffic on interface
ls -l /sys/fs/cgroup/mocker # verify cgroup for mocker process
cat /sys/fs/cgroup/mocker/cgroup.procs # verify process matched mocker (from `ps aux | grep mocker`)
# exit container and verify cleanup
ip link ls | grep veth0     # should show nothing (successfully cleaned up when container stops)
```

## Current Limitations

- No image handling (uses local busybox only)
- No user namespace isolation
- Minimal command set through busybox
- No persistent storage

## Future Improvements

Possible enhancements:

- Container image support
- User namespace support
- Support for persistent volumes
- Use libmnl to set NAT rules :question: (not sure how tho :confused:)

## Security Notes

This is a educational implementation and lacks many security features of production container runtimes. Do not use on your host machine directly! Use a virtual machine!

## License

This project is provided as-is for educational purposes.

[1]: public/mocker-demo.gif 'Demo of mocker container running'
