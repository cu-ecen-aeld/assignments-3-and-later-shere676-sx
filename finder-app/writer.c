#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <syslog.h>

static ssize_t write_full(int fd, const char *buf, size_t count)
{
    size_t total_written = 0;

    while (total_written < count)
    {
        ssize_t n = write(fd, buf + total_written, count - total_written);

        if (n == -1)
        {
            if (errno == EINTR)
            {
                continue; 
            }
            return -1;
        }

        total_written += (size_t)n;
    }

    return (ssize_t)total_written;
}

int main(int argc, char *argv[])
{
    const char *writefile;
    const char *writestr;
    int fd;
    size_t len;
    int rc = 0;

    
    openlog("writer", LOG_PID, LOG_USER);


    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <writefile> <writestr>\n", argv[0]);
        syslog(LOG_ERR, "Invalid arguments: expected 2, got %d", argc - 1);
        closelog();
        return 1;
    }

    writefile = argv[1];
    writestr = argv[2];


    fd = open(writefile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1)
    {
        fprintf(stderr, "Error: could not create %s: %s\n", writefile, strerror(errno));
        syslog(LOG_ERR, "Could not open/create %s: %s", writefile, strerror(errno));
        closelog();
        return 1;
    }

   
    syslog(LOG_DEBUG, "Writing %s to %s", writestr, writefile);

    len = strlen(writestr);
    if (write_full(fd, writestr, len) == -1)
    {
        fprintf(stderr, "Error: could not write to %s: %s\n", writefile, strerror(errno));
        syslog(LOG_ERR, "Write to %s failed: %s", writefile, strerror(errno));
        rc = 1;
    }

    if (close(fd) == -1)
    {
        fprintf(stderr, "Error: could not close %s: %s\n", writefile, strerror(errno));
        syslog(LOG_ERR, "close() on %s failed: %s", writefile, strerror(errno));
        rc = 1;
    }

    closelog();
    return rc;
}