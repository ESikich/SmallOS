#ifndef USER_SEMAPHORE_WRAPPER_H
#define USER_SEMAPHORE_WRAPPER_H

typedef struct {
    int value;
} sem_t;

static inline int sem_init(sem_t* sem, int pshared, unsigned int value) {
    (void)pshared;
    if (!sem) return -1;
    sem->value = (int)value;
    return 0;
}

static inline int sem_wait(sem_t* sem) {
    if (!sem) return -1;
    if (sem->value <= 0) return -1;
    sem->value--;
    return 0;
}

static inline int sem_post(sem_t* sem) {
    if (!sem) return -1;
    sem->value++;
    return 0;
}

#endif
