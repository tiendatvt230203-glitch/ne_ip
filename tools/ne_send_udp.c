#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NE_MAGIC "NE1UDP_ip_probe________"

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [src_ip] [dst_ip] [dst_port] [payload_bytes]\n"
            "  Default: 192.168.9.2 -> 192.168.180.2:12345 payload=500\n",
            prog);
}

int main(int argc, char **argv) {
    const char *src_ip = "192.168.9.2";
    const char *dst_ip = "192.168.180.2";
    int dst_port = 12345;
    int payload_len = 500;

    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        usage(argv[0]);
        return 0;
    }
    if (argc >= 2) src_ip = argv[1];
    if (argc >= 3) dst_ip = argv[2];
    if (argc >= 4) dst_port = atoi(argv[3]);
    if (argc >= 5) payload_len = atoi(argv[4]);
    if (payload_len < 64 || payload_len > 9000) {
        fprintf(stderr, "payload_bytes must be 64..9000\n");
        return 1;
    }

    char *buf = calloc(1, (size_t)payload_len);
    if (!buf) {
        perror("calloc");
        return 1;
    }
    memset(buf, 'X', (size_t)payload_len);
    memcpy(buf, NE_MAGIC, strlen(NE_MAGIC));

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        free(buf);
        return 1;
    }

    struct sockaddr_in src;
    memset(&src, 0, sizeof(src));
    src.sin_family = AF_INET;
    src.sin_port = htons(0);
    if (inet_pton(AF_INET, src_ip, &src.sin_addr) == 1)
        bind(sock, (struct sockaddr *)&src, sizeof(src));

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)dst_port);
    if (inet_pton(AF_INET, dst_ip, &dst.sin_addr) != 1) {
        fprintf(stderr, "bad dst ip\n");
        close(sock);
        free(buf);
        return 1;
    }

    ssize_t n = sendto(sock, buf, (size_t)payload_len, 0,
                       (struct sockaddr *)&dst, sizeof(dst));
    close(sock);
    free(buf);

    if (n != payload_len) {
        perror("sendto");
        return 1;
    }

    fprintf(stderr, "OK: UDP %dB %s -> %s:%d\n", payload_len, src_ip, dst_ip, dst_port);
    return 0;
}
