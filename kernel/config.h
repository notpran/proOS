#ifndef CONFIG_H
#define CONFIG_H

/*
 * Global kernel configuration knobs.
 * Adjusting these values enables stress and scalability testing without
 * having to patch core kernel sources.
 */
#define CONFIG_MAX_PROCS          32
#define CONFIG_PROC_STACK_SIZE    4096
#define CONFIG_MSG_QUEUE_LEN      16
#define CONFIG_MSG_DATA_MAX       256

#define CONFIG_KLOG_CAPACITY      128
#define CONFIG_KLOG_ENTRY_LEN     96
#define CONFIG_KLOG_DEFAULT_LEVEL 1

#define CONFIG_STRESS_SPIN_CYCLES 5000

#endif
