#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [src_ip] [dst_ip] [dst_port]\n"
            "  Default: 192.168.9.2 -> 192.168.180.2:22 (SSH through NE-IP)\n",
            prog);
}

int main(int argc, char **argv) {
    const char *src_ip = "192.168.9.2";
    const char *dst_ip = "192.168.180.2";
    int dst_port = 22;

    if (argc > 1) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
    }
    if (argc >= 2) src_ip = argv[1];
    if (argc >= 3) dst_ip = argv[2];
    if (argc >= 4) dst_port = atoi(argv[3]);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in src;
    memset(&src, 0, sizeof(src));
    src.sin_family = AF_INET;
    src.sin_port = htons(0);
    if (inet_pton(AF_INET, src_ip, &src.sin_addr) == 1) {
        if (bind(fd, (struct sockaddr *)&src, sizeof(src)) < 0)
            fprintf(stderr, "[WARN] bind %s: %s\n", src_ip, strerror(errno));
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)dst_port);
    if (inet_pton(AF_INET, dst_ip, &dst.sin_addr) != 1) {
        fprintf(stderr, "bad dst ip: %s\n", dst_ip);
        close(fd);
        return 1;
    }

    if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        fprintf(stderr, "connect %s:%d failed: %s\n", dst_ip, dst_port, strerror(errno));
        close(fd);
        return 1;
    }

    char banner[256];
    ssize_t n = read(fd, banner, sizeof(banner) - 1);
    if (n > 0) {
        banner[n] = '\0';
        fprintf(stderr, "OK: TCP+SSH banner (%zd bytes) %s -> %s:%d\n", n, src_ip, dst_ip, dst_port);
        fwrite(banner, 1, (size_t)n, stderr);
        if (banner[n - 1] != '\n')
            fputc('\n', stderr);
    } else {
        fprintf(stderr, "OK: TCP connected %s -> %s:%d (no banner yet)\n", src_ip, dst_ip, dst_port);
    }

    close(fd);
    return 0;
}
