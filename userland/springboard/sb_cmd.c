#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <cmd>\n", argv[0]);
        return 1;
    }
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return 1; }

    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, "/var/run/sb.sock", sizeof(sun.sun_path)-1);

    if (connect(s, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
        // Fallback to /tmp/sb.sock if /var/run/sb.sock fails
        strncpy(sun.sun_path, "/tmp/sb.sock", sizeof(sun.sun_path)-1);
        if (connect(s, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
            perror("connect /var/run/sb.sock");
            close(s);
            return 1;
        }
    }

    char cmd[256] = {0};
    snprintf(cmd, sizeof(cmd), "%s", argv[1]);
    for (int i = 2; i < argc; i++) {
        strncat(cmd, " ", sizeof(cmd)-strlen(cmd)-1);
        strncat(cmd, argv[i], sizeof(cmd)-strlen(cmd)-1);
    }
    strncat(cmd, "\n", sizeof(cmd)-strlen(cmd)-1);

    write(s, cmd, strlen(cmd));

    char buf[1024] = {0};
    ssize_t n = read(s, buf, sizeof(buf)-1);
    if (n > 0) {
        buf[n] = 0;
        printf("Response: %s", buf);
    } else {
        printf("No response from SpringBoard\n");
    }
    close(s);
    return 0;
}
