#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include "common.h"

/**
 * @brief Logs a message to the server log file with a timestamp.
 * @param msg The message string to log.
 */
void log_activity(const char *msg) {
    static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&log_mutex);
    FILE *f = fopen(LOG_FILE, "a");
    if (f == NULL) {
        perror("Cannot open log file");
        pthread_mutex_unlock(&log_mutex);
        return;
    }

    // Get current time
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char time_str[64];
    // Format: YYYY-MM-DD HH:MM:SS
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &local);

    // Write to file
    fprintf(f, "[%s] %s\n", time_str, msg);
    
    // Also print to server console for monitoring
    printf("[%s] %s\n", time_str, msg);

    fclose(f);
    pthread_mutex_unlock(&log_mutex);
}
