/*
 * test_app.c - Ordinary user-space program exercising /dev/mychardev
 *
 * Compile with a plain gcc invocation (see README.md) -- no kernel
 * headers or special flags needed, since this runs entirely in user
 * space against normal glibc/POSIX APIs.
 */

#include <stdio.h>      // printf(), perror()
#include <stdlib.h>     // exit(), EXIT_FAILURE, EXIT_SUCCESS
#include <fcntl.h>      // open(), O_RDWR
#include <unistd.h>     // write(), read(), close(), lseek()
#include <string.h>     // strlen(), memset()
#include <errno.h>      // errno (used internally by perror)

#define DEVICE_PATH "/dev/mychardev"
#define BUFFER_SIZE 1024

int main(void)
{
    int fd;
    char write_msg[] = "Hello from user space!";
    char read_buf[BUFFER_SIZE];
    ssize_t bytes_written, bytes_read;
    off_t seek_pos;

    /* Step 1: Open the device in read-write mode. Triggers VFS -> inode
     * lookup -> mychardev_open() on the kernel side. */
    fd = open(DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        perror("Failed to open device");
        exit(EXIT_FAILURE);
    }
    printf("[test_app] Device opened successfully, fd = %d\n", fd);

    /* Step 2: Write a message. strlen() excludes the null terminator --
     * our driver's buffer just stores raw bytes, not a C string. */
    bytes_written = write(fd, write_msg, strlen(write_msg));
    if (bytes_written < 0) {
        perror("Failed to write to device");
        close(fd);
        exit(EXIT_FAILURE);
    }
    printf("[test_app] Wrote %zd bytes: \"%s\"\n", bytes_written, write_msg);

    /* Step 3: write() advanced the file position -- seek back to 0 so
     * the upcoming read() reads what we just wrote, not stale data. */
    seek_pos = lseek(fd, 0, SEEK_SET);
    if (seek_pos < 0) {
        perror("Failed to seek device");
        close(fd);
        exit(EXIT_FAILURE);
    }
    printf("[test_app] Seeked back to position %ld\n", (long)seek_pos);

    /* Step 4: Clear our own stack buffer first so any bytes not
     * overwritten by read() don't look like driver output. */
    memset(read_buf, 0, BUFFER_SIZE);

    /* Step 5: Read back exactly what we wrote. */
    bytes_read = read(fd, read_buf, bytes_written);
    if (bytes_read < 0) {
        perror("Failed to read from device");
        close(fd);
        exit(EXIT_FAILURE);
    }
    printf("[test_app] Read %zd bytes: \"%s\"\n", bytes_read, read_buf);

    /* Step 6: Exercise SEEK_END and SEEK_CUR too, not just SEEK_SET. */
    seek_pos = lseek(fd, 0, SEEK_END);
    printf("[test_app] SEEK_END position: %ld\n", (long)seek_pos);

    seek_pos = lseek(fd, -5, SEEK_CUR);
    printf("[test_app] SEEK_CUR (-5) position: %ld\n", (long)seek_pos);

    /* Step 7: Close -- triggers mychardev_release() on the kernel side. */
    if (close(fd) < 0) {
        perror("Failed to close device");
        exit(EXIT_FAILURE);
    }
    printf("[test_app] Device closed successfully\n");

    return EXIT_SUCCESS;
}
