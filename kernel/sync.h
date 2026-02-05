#ifndef SYNC_H
#define SYNC_H

#include <stdint.h>

void sync_init(void);

int sync_mutex_create(void);
int sync_mutex_lock(int id);
int sync_mutex_unlock(int id);

int sync_semaphore_create(int initial_count);
int sync_semaphore_wait(int id);
int sync_semaphore_post(int id);

#endif
