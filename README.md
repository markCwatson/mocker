# _mocker_: a minimal container runtime in C

This project implements a basic container runtime in C that demonstrates core container concepts like process isolation, filesystem isolation, and mount namespaces.

![alt-text][1]

## Features

- **Process Isolation**: Uses Linux namespaces (PID, Mount, UTS, IPC) to isolate processes
- **Filesystem Isolation**: Creates an isolated filesystem using chroot
- **Mount Namespace**: Sets up essential filesystem mounts (proc, sys, dev)
- **Minimal Root Filesystem**: Uses busybox to provide basic Unix utilities
- **Cleanup**: Properly unmounts filesystems and cleans up resources

## Requirements

- Linux system with namespace support
- GCC compiler
- busybox-static package
- Root privileges (for namespace operations)
- (optional) Docker

## Building

```shell
# Install required packages (on Debian/Ubuntu)
sudo apt-get update
sudo apt-get install -y build-essential gcc busybox-static

# Compile with full debugging symbols (use -DENABLE_LOGGING to enable logging)
gcc -g -o mocker app/*.c -lcurl
```

## Usages

### Option 1: Linux VM

You can run `mocker` inside a VM running Linux (tested with Kali) and execute a limited set of commands inside the _mocker_ container.

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

# Not implemented:
sudo ./mocker run ubuntu:latest /bin/ping -c 4 8.8.8.8
```

### Option 2: Using Docker (wip)

I am developing on a M4 Mac and am having issues related to the use of the x86 version of busybox.

Add a shell alias (i.e. `nano ~/.zshrc`).

```shell
alias mocker='docker build --platform linux/arm64 -t mocker . && docker run --platform linux/arm64 --privileged --cap-add="ALL" --device=/dev/net/tun --network=host mocker'
```

You can now test the program like this:

```shell
# run basic tests:
mocker run ubuntu:latest /bin/ls /
mocker run ubuntu:latest /bin/ps
mocker run ubuntu:latest /bin/echo "Hello from container"

# not implemented:
mocker run ubuntu:latest /bin/ping -c 4 8.8.8.8
```

Cleanup.

```shell
docker rmi mocker -f
```

## Current Limitations

- No image handling (uses local busybox only)
- No network namespace isolation
- No resource limits (cgroups not implemented)
- No user namespace isolation
- Minimal command set through busybox
- No persistent storage

## Future Improvements

Possible enhancements:

- Add container image support
- Implement network isolation
- Add cgroups for resource control
- Add user namespace support
- Support for persistent volumes
- Add more security features

## Security Notes

This is a educational implementation and lacks many security features of production container runtimes. Do not use on your host machine directly!

## License

This project is provided as-is for educational purposes.

[1]: gif/mocker-demo.gif 'Demo of mocker container running'
