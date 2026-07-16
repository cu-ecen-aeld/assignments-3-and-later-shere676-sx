#include "threading.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

// Optional: use these functions to add debug or error prints to your application
#define DEBUG_LOG(msg,...)
//#define DEBUG_LOG(msg,...) printf("threading: " msg "\n" , ##__VA_ARGS__)
#define ERROR_LOG(msg,...) printf("threading ERROR: " msg "\n" , ##__VA_ARGS__)

/**
 * Sleep for the given number of milliseconds using nanosleep, which (unlike
 * usleep) has no restriction requiring the wait to be under one second.
 * Returns 0 on success, -1 on error (matching nanosleep's return contract).
 */
static int sleep_ms(int milliseconds)
{
    if (milliseconds <= 0) {
        return 0;
    }
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000L;
    return nanosleep(&ts, NULL);
}

void* threadfunc(void* thread_param)
{
    struct thread_data* thread_func_args = (struct thread_data *) thread_param;

    // Default to failure until every step below succeeds
    thread_func_args->thread_complete_success = false;

    DEBUG_LOG("waiting %d ms before obtaining mutex", thread_func_args->wait_to_obtain_ms);
    if (sleep_ms(thread_func_args->wait_to_obtain_ms) != 0) {
        ERROR_LOG("sleep before obtaining mutex failed");
        return thread_param;
    }

    int rc = pthread_mutex_lock(thread_func_args->mutex);
    if (rc != 0) {
        ERROR_LOG("pthread_mutex_lock failed with %d", rc);
        return thread_param;
    }

    DEBUG_LOG("mutex obtained, waiting %d ms before releasing", thread_func_args->wait_to_release_ms);
    if (sleep_ms(thread_func_args->wait_to_release_ms) != 0) {
        ERROR_LOG("sleep before releasing mutex failed");
        // Still attempt to unlock so we don't leave the mutex permanently locked
        pthread_mutex_unlock(thread_func_args->mutex);
        return thread_param;
    }

    rc = pthread_mutex_unlock(thread_func_args->mutex);
    if (rc != 0) {
        ERROR_LOG("pthread_mutex_unlock failed with %d", rc);
        return thread_param;
    }

    thread_func_args->thread_complete_success = true;
    return thread_param;
}


bool start_thread_obtaining_mutex(pthread_t *thread, pthread_mutex_t *mutex,int wait_to_obtain_ms, int wait_to_release_ms)
{
    struct thread_data *thread_func_args = (struct thread_data *) malloc(sizeof(struct thread_data));
    if (thread_func_args == NULL) {
        ERROR_LOG("malloc failed to allocate thread_data");
        return false;
    }

    thread_func_args->mutex = mutex;
    thread_func_args->wait_to_obtain_ms = wait_to_obtain_ms;
    thread_func_args->wait_to_release_ms = wait_to_release_ms;
    thread_func_args->thread_complete_success = false;

    int rc = pthread_create(thread, NULL, threadfunc, (void *) thread_func_args);
    if (rc != 0) {
        ERROR_LOG("pthread_create failed with %d", rc);
        free(thread_func_args);
        return false;
    }

    return true;
}