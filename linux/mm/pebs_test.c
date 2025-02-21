// htmm_pebs_module.c
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/perf_event.h>
#include <linux/sched.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/sched/cputime.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/cgroup.h>

#include "../kernel/events/internal.h"

#include <linux/pebs_test.h>

#define CPUS_PER_SOCKET 64
#define BUFFER_SIZE	4096 /* 128: 1MB */
#define SAMPLE_PERIOD 1000

/* pebs events */
#define DRAM_LLC_LOAD_MISS  0x1d3
#define REMOTE_DRAM_LLC_LOAD_MISS   0x2d3
#define NVM_LLC_LOAD_MISS   0x80d1
#define ALL_STORES	    0x82d0
#define ALL_LOADS	    0x81d0
#define STLB_MISS_STORES    0x12d0
#define STLB_MISS_LOADS	    0x11d0
#define LLC_LOAD_MISS 0x20d1

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sungsu Ahn");
MODULE_DESCRIPTION("PEBS Sampling Module");

static struct task_struct *access_sampling;
static struct perf_event ***mem_event;
static struct VMAArray *vma_array_ptr;

static bool valid_va(unsigned long addr)
{
    if (!(addr >> (PGDIR_SHIFT + 9)) && addr != 0)
	return true;
    else
	return false;
}

struct test_event {
    struct perf_event_header header;
    __u64 ip;
    __u32 pid, tid;
    __u64 addr;
    __u64 phys_addr;
};

enum events {
    LLC_LOAD = 0,
    ALL_STORE = 1,
    N_PEBSEVENTS
};

static __u64 get_pebs_event(enum events e)
{
    switch (e) {
    case LLC_LOAD:
        return LLC_LOAD_MISS;
    case ALL_STORE:
        return ALL_STORES;
	default:
	    return N_PEBSEVENTS;
    }
}

static int __perf_event_open(__u64 config, __u64 cpu, __u64 type, pid_t pid, int cgroup_fd)
{
    struct perf_event_attr attr;
    struct file *file;
    int event_fd;

    memset(&attr, 0, sizeof(struct perf_event_attr));

    attr.type = PERF_TYPE_RAW;
    attr.size = sizeof(struct perf_event_attr);
    attr.config = config;
	attr.sample_period = SAMPLE_PERIOD;
    attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_ADDR | PERF_SAMPLE_PHYS_ADDR;
    attr.disabled = 0;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.exclude_callchain_kernel = 1;
    attr.exclude_callchain_user = 1;
    attr.precise_ip = 2;
    attr.enable_on_exec = 1;
    attr.inherit = 1;

    printk("[__perf_event_open] pid: %d, cpu: %lld, type: %lld\n", pid, cpu, type);
	
    event_fd = test__perf_event_open(&attr, cgroup_fd, cpu, -1, PERF_FLAG_PID_CGROUP);
    // event_fd = test__perf_event_open(&attr, pid, cpu, -1, 0);
    // event_fd = test__perf_event_open(&attr, -1, cpu, -1, 0);
    if (event_fd <= 0) {
        printk("[error test__perf_event_open failure] event_fd: %d\n", event_fd);
        return -1;
    }

    file = fget(event_fd);
    if (!file) {
	printk("invalid file\n");
	return -1;
    }
    mem_event[cpu][type] = fget(event_fd)->private_data;
    return 0;
}

static int pebs_init(pid_t pid, int cgroup_fd)
{
    int cpu, event;

    printk(KERN_INFO "[pebs_init] start\n");

    mem_event = kzalloc(sizeof(struct perf_event **) * CPUS_PER_SOCKET, GFP_KERNEL);
    for (cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {
	mem_event[cpu] = kzalloc(sizeof(struct perf_event *) * N_PEBSEVENTS, GFP_KERNEL);
    }

    printk(KERN_INFO "[pebs_init] mem_event initialized \n");

    for (cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {       
        // to disable PEBS of node 1 cpus
        if ((cpu >= 16 && cpu < 32) || (cpu >= 48 && cpu < 64)) {
            printk(KERN_INFO "Disable PEBS of node 1 CPU: %d\n", cpu);
            continue;
        }
        for (event = 0; event < N_PEBSEVENTS; event++) {
            if (get_pebs_event(event) == N_PEBSEVENTS) {
            mem_event[cpu][event] = NULL;
            continue;
            }
            
            printk(KERN_INFO "Creating PEBS event for CPU %d, event %d\n", cpu, event);
            if (__perf_event_open(get_pebs_event(event), cpu, event, pid, cgroup_fd)) return -1;
            if (test__perf_event_init(mem_event[cpu][event], BUFFER_SIZE)) return -1;
            printk(KERN_INFO "PEBS event created for event %d\n", event);
        }
    }
    return 0;
}

static void pebs_cleanup(void)
{
    int cpu, event;
    printk(KERN_INFO "pebs_cleanup called\n");
    for (cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {
        for (event = 0; event < N_PEBSEVENTS; event++) {
            if (mem_event[cpu][event]) {
                if (!mem_event[cpu][event]) {
                    printk(KERN_INFO "No PEBS event for CPU %d, event %d\n", cpu, event);
                    return;
                }
                printk(KERN_INFO "Disabling PEBS event for CPU %d, event %d\n", cpu, event);
                perf_event_disable(mem_event[cpu][event]);
            }
        }
        // kfree(mem_event[cpu]);
    }
}

static int ksamplingd(void *data)
{
    printk(KERN_INFO "[ksamplingd] Sampling thread started\n");
    while (!kthread_should_stop()) {
        int cpu, event, cond = false;
        
        for (cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {
            for (event = 0; event < N_PEBSEVENTS; event++) {
                do {
                    struct perf_buffer *rb;
                    struct perf_event_mmap_page *up;
                    struct perf_event_header *ph;
                    struct test_event *te;
                    unsigned long pg_index, offset;
                    int page_shift;
                    __u64 head;

                    if (!mem_event[cpu][event]) {
                        // printk(KERN_INFO "[ksamplingd] No event for CPU %d, event %d\n", cpu, event);
                        break;
                    }

                    __sync_synchronize();

                    rb = mem_event[cpu][event]->rb;
                    if (!rb) {
                        // printk(KERN_INFO "[ksamplingd] Ring buffer is NULL for CPU %d, event %d\n", cpu, event);
                        return -1;
                    }

                    up = READ_ONCE(rb->user_page);
                    head = READ_ONCE(up->data_head);
                    if (head == up->data_tail) {
                        // printk(KERN_INFO "[ksamplingd] No new data for CPU %d, event %d\n", cpu, event);
                        break;
                    }

                    head -= up->data_tail;
                    if (head > (BUFFER_SIZE * 50 / 100)) {
                        // printk(KERN_INFO "[ksamplingd] Buffer more than 50%% full (size: %llu)\n", head);
                        cond = true;
                    } else if (head < (BUFFER_SIZE * 10 / 100)) {
                        // printk(KERN_INFO "[ksamplingd] Buffer less than 10%% full (size: %llu)\n", head);
                        // printk(KERN_INFO "[ksamplingd] cond to false\n");
                        cond = false;
                    }

                    smp_rmb();

                    page_shift = PAGE_SHIFT + page_order(rb);
                    offset = READ_ONCE(up->data_tail);
                    pg_index = (offset >> page_shift) & (rb->nr_pages - 1);
                    offset &= (1 << page_shift) - 1;

                    ph = (void*)(rb->data_pages[pg_index] + offset);
                    switch (ph->type) {
                    case PERF_RECORD_SAMPLE:
                        te = (struct test_event *)ph;
                        if (!valid_va(te->addr)) {
                            // printk(KERN_INFO "[ksamplingd] Invalid virtual address detected\n");
                            break;
                        }

                        // printk(KERN_INFO "[ksamplingd] PEBS sample: ip=0x%llx, pid=%d, tid=%d, addr=0x%llx, phys_addr=0x%llx\n",
                        //     te->ip, te->pid, te->tid, te->addr, te->phys_addr);
                        access_address(vma_array_ptr, te->addr);
                        break;
                    case PERF_RECORD_THROTTLE:
                    case PERF_RECORD_UNTHROTTLE:
                        // printk(KERN_INFO "[ksamplingd] Throttle event detected\n");
                        break;
                    case PERF_RECORD_LOST_SAMPLES:
                        // printk(KERN_INFO "[ksamplingd] Lost samples event detected\n");
                        break;
                    default:
                        // printk(KERN_INFO "[ksamplingd] Unknown event type: %d\n", ph->type);
                        break;
                    }
                    smp_mb();
                    WRITE_ONCE(up->data_tail, up->data_tail + ph->size);
                } while (cond);
            }
        }
        msleep_interruptible(100);
    }
    printk(KERN_INFO "[ksamplingd] Sampling thread stopped\n");
    return 0;
}

int pebs_test_init(pid_t pid, char* cgroup_path)
{
    printk(KERN_INFO "PEBS test initializing\n");
    int ret;
    // print_all_vma(pid);
    vma_array_ptr = vma_array_init(pid);
    printk(KERN_INFO "VMA array initialized\n");
    
    //for monitoring cgroup
    struct file *cgroup_file = filp_open(cgroup_path, O_RDONLY, 0);
    if (IS_ERR(cgroup_file)) {
        printk("[error open failure]\n");
        return -1;
    }
    int cgroup_fd = get_unused_fd_flags(0);
    if (cgroup_fd < 0) {
        printk(KERN_ERR "Error get_unused_fd_flags\n");
        return -1;
    }
    fd_install(cgroup_fd, cgroup_file);
    printk(KERN_INFO "cgroup_fd: %d\n", cgroup_fd);
    
    //pebs_init
    if ((ret = pebs_init(pid, cgroup_fd)) != 0)
        return ret;

    if (access_sampling) {
        printk(KERN_INFO "Sampling thread already exists\n");
        return -1;
    }
    //run sampling thread
    access_sampling = kthread_run(ksamplingd, NULL, "pebs_sampling");
    if (IS_ERR(access_sampling)) {
        pebs_cleanup();
        return PTR_ERR(access_sampling);
    }
    
    printk(KERN_INFO "PEBS test initialized\n");
    return 0;
}

void pebs_test_exit(void)
{
    if (!access_sampling) {
        printk(KERN_INFO "Sampling thread does not exist\n");
        return;
    }
    printk(KERN_INFO "Sampling thread stopping...\n");
    kthread_stop(access_sampling);
    printk(KERN_INFO "Sampling thread stopped\n");
    
    if (!vma_array_ptr) {
        printk(KERN_INFO "VMA array does not exist\n");
        return;
    }
    printk(KERN_INFO "VMA array statistics\n");
    vma_array_stat(vma_array_ptr);

    printk(KERN_INFO "Call free_vma_array\n");
    free_vma_array(vma_array_ptr);

    printk(KERN_INFO "Call pebs_cleanup\n");
    pebs_cleanup();

    printk(KERN_INFO "pebs_cleanup done.\n");
}
