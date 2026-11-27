#ifndef ASPHALT6_THREAD_WIN32_H
#define ASPHALT6_THREAD_WIN32_H

#include <stdint.h>

int thread_win32_create(uint8_t *mem, uint32_t mem_size, uint32_t thread_ptr,
                        uint32_t start_routine, uint32_t arg);
int thread_win32_join(uint8_t *mem, uint32_t thread_id, uint32_t retval_ptr);
uint32_t thread_win32_self(void);

int thread_win32_mutex_init(uint8_t *mem, uint32_t mem_size, uint32_t mutex_ptr);
int thread_win32_mutex_lock(uint8_t *mem, uint32_t mem_size, uint32_t mutex_ptr);
int thread_win32_mutex_trylock(uint8_t *mem, uint32_t mem_size, uint32_t mutex_ptr);
int thread_win32_mutex_unlock(uint8_t *mem, uint32_t mem_size, uint32_t mutex_ptr);
int thread_win32_mutex_destroy(uint8_t *mem, uint32_t mem_size, uint32_t mutex_ptr);

int thread_win32_cond_init(uint8_t *mem, uint32_t mem_size, uint32_t cond_ptr);
int thread_win32_cond_signal(uint8_t *mem, uint32_t mem_size, uint32_t cond_ptr);
int thread_win32_cond_broadcast(uint8_t *mem, uint32_t mem_size, uint32_t cond_ptr);
int thread_win32_cond_wait(uint8_t *mem, uint32_t mem_size, uint32_t cond_ptr, uint32_t mutex_ptr);
int thread_win32_cond_timedwait(uint8_t *mem, uint32_t mem_size, uint32_t cond_ptr,
                                uint32_t mutex_ptr, uint32_t abstime_ptr);
int thread_win32_cond_destroy(uint8_t *mem, uint32_t mem_size, uint32_t cond_ptr);

int thread_win32_key_create(uint8_t *mem, uint32_t mem_size, uint32_t key_ptr, uint32_t dtor);
uint32_t thread_win32_getspecific(uint32_t key);
int thread_win32_setspecific(uint32_t key, uint32_t val);

#endif /* ASPHALT6_THREAD_WIN32_H */
