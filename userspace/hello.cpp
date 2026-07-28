#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>

extern "C" void _start() {
    printf("=======================================\n");
    printf("Hello from Ring 3 using drakos libc!\n");       // idk lowercase looks better ngl
    printf("PID: %d\n", getpid());
    printf("=======================================\n");

    // Test malloc
    char* buf = (char*)malloc(1024);
    if (buf) {
        printf("Malloc successful, allocated at %p\n", buf);
    } else {
        printf("Malloc failed!\n");
    }

    printf("Sleeping for 3 seconds...\n");
    sleep(3);
    printf("Woke up from sleep!\n");

    // Test VFS syscalls
    int fd = open("/nvme/HELLO.DRK", 0);
    if (fd >= 0) {
        printf("Successfully opened /nvme/HELLO.DRK (fd %d)\n", fd);
        
        if (buf) {
            ssize_t bytes = read(fd, buf, 64);
            printf("Read %d bytes from file.\n", (int)bytes);
        }

        close(fd);
    } else {
        printf("Failed to open file! (ret: %d)\n", fd);
    }

    printf("Exiting gracefully...\n");
    exit(0);
}
