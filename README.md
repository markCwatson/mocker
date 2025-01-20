# _mocker_: a minimal container runtime in C

This project implements a basic container runtime in C that demonstrates core container concepts like process isolation, filesystem isolation, and mount namespaces.

![alt-text][1]

## Features

- **Process Isolation**: Makes use of Linux namespaces (PID, Mount, UTS, IPC) to isolate processes using [clone](https://man7.org/linux/man-pages/man2/clone.2.html) and the appropriate flags when creating the child process.
- **Filesystem Isolation**: Creates an isolated filesystem environment by creating necessary directories, copying the [BusyBox](https://www.busybox.net/downloads/BusyBox.html) binary, and mapping symlinks to busybox utilities. It then uses [chroot](https://man7.org/linux/man-pages/man2/chroot.2.html) change the root filesystem, ensuring the child process operates within its own filesystem.
- **Mount Namespace**: Configures essential filesystem mounts, including `proc`, `sys`, and `tmpfs`, within the container's root filesystem. This is done using the [mount](https://man7.org/linux/man-pages/man2/mount.2.html) system call to ensure the container has access to necessary system information and temporary storage.
- **Minimal Root Filesystem**: Uses busyBox to provide a minimal set of Unix utilities within the container. This involves copying the BusyBox binary to the container's `bin` directory and creating symbolic links for various utilities.
- **Cleanup**: Ensures proper unmounting of filesystems and cleanup of resources to maintain system integrity using [umount2](https://man7.org/linux/man-pages/man2/umount.2.html). This involves unmounting the `proc`, `sys`, and `tmpfs` filesystems and removing any temporary directories created during the setup.

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
gcc -g -o mocker -Iapp app/*.c -lcurl
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
