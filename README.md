## Container runtime in C

This project is a custom implementation of a _Docker-like_ container runtime in C. It SHOWS how to create and manage lightweight, isolated environments for running applications, similar to how Docker operates. The project includes functionality for setting up network interfaces, creating process namespaces, and managing system resources within containers.

Features:

- **Network Interface Management:** The project sets up virtual Ethernet (veth) pairs to enable network communication between the host and the container. It includes functions to clean up existing network interfaces and create new ones.

- **Namespace Isolation:** The project uses Linux namespaces to isolate various aspects of the container environment, such as process IDs (PID), mount points (NEWNS), UTS (hostname), IPC (inter-process communication), and network (NEWNET). This isolation ensures that the container operates independently of the host system.

- **Process Management:** The project uses the clone system call to create new processes within the isolated namespaces. It sets up a child process with its own stack and passes necessary arguments to it.

- **Error Handling:** The project includes robust error handling to manage failures in memory allocation, network interface creation, and process cloning.

### Building and running

We use linux-specific syscalls, so we'll run the code
in a Docker container.

Must have [Docker installed](https://docs.docker.com/get-docker/).

Next, add a shell alias (i.e. `nano ~/.zshrc`).

```sh
alias mydocker='docker build --platform linux/arm64 -t mydocker . && docker run --platform linux/arm64 --privileged --cap-add="ALL" --device=/dev/net/tun --network=host mydocker'
```

You can now execute the program like this:

```sh
mydocker run alpine:latest /bin/ping -c 4 8.8.8.8
```
