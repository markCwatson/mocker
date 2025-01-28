#include "cgroup.h"
#include "logging.h"

#define MEMORY_LIMIT (1024 * 1024 * 1024)
#define CPU_LIMIT 100000

// private data structure
struct cgroup_config_s
{
    int memory_limit;
    int cpu_limit;
    pid_t child_pid;
    const char *cgroup;
};

// private global variable for cgroup config
struct cgroup_config_s cgroup_config = {
    .memory_limit = MEMORY_LIMIT,
    .cpu_limit = CPU_LIMIT,
    .child_pid = -1,
    .cgroup = NULL,
};

int setup_cgroup(pid_t child_pid)
{
    LOG("[CGROUP] Setting up cgroup\n");
    cgroup_config.child_pid = child_pid;
    cgroup_config.cgroup = "/sys/fs/cgroup/mocker";
    FILE *f = NULL;

    LOG("[CGROUP] Creating cgroup\n");
    if (mkdir(cgroup_config.cgroup, 0755) == -1)
    {
        LOG("[CGROUP] Failed to create cgroup: %s\n", strerror(errno));
        return -1;
    }

    LOG("[CGROUP] Memory limit: %d\n", cgroup_config.memory_limit);
    LOG("[CGROUP] CPU limit: %d\n", cgroup_config.cpu_limit);

    char cg_memory[256];
    snprintf(cg_memory, sizeof(cg_memory), "%s/memory.max", cgroup_config.cgroup);
    f = fopen(cg_memory, "w");
    if (f == NULL)
    {
        LOG("[CGROUP] Failed to open memory.max: %s\n", strerror(errno));
        return -1;
    }

    fprintf(f, "%d", cgroup_config.memory_limit);
    fclose(f);

    char cg_cpu[256];
    snprintf(cg_cpu, sizeof(cg_cpu), "%s/cpu.max", cgroup_config.cgroup);
    f = fopen(cg_cpu, "w");
    if (f == NULL)
    {
        LOG("[CGROUP] Failed to open cpu.max: %s\n", strerror(errno));
        return -1;
    }
    fprintf(f, "%d", cgroup_config.cpu_limit);
    fclose(f);

    LOG("[CGROUP] Assigning child process to cgroup\n");
    char cg_procs[256];
    snprintf(cg_procs, sizeof(cg_procs), "%s/cgroup.procs", cgroup_config.cgroup);
    f = fopen(cg_procs, "w");
    if (f == NULL)
    {
        LOG("[CGROUP] Failed to open cgroup.procs: %s\n", strerror(errno));
        return -1;
    }
    fprintf(f, "%d", cgroup_config.child_pid);
    fclose(f);

    LOG("[CGROUP] Cgroup setup complete\n");
    return 0;
}

int cleanup_cgroup(void)
{
    rmdir(cgroup_config.cgroup);
    LOG("[CGROUP] Cgroup cleaned up\n");
    return 0;
}