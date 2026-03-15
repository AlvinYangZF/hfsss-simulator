/*
 * hfsss-ctrl — command-line interface for the HFSSS OOB management socket
 * (REQ-129)
 *
 * Usage:
 *   hfsss-ctrl [-s /path/to/hfsss.sock] <command> [args...]
 *
 * Commands:
 *   status          — show simulator status
 *   smart           — show SMART health log (16 fields)
 *   perf            — show performance counters
 *   perf reset      — reset rolling window counters
 *   gc trigger      — request immediate GC cycle
 *   trace enable    — enable command tracing
 *   trace disable   — disable command tracing
 *   log             — dump recent log entries
 *   config get      — show current configuration
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

#define DEFAULT_SOCK  "/tmp/hfsss.sock"
#define BUF_SIZE      8192

static int g_verbose = 0;

static int connect_sock(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "error: cannot connect to %s: %s\n",
                path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int send_recv(int fd, const char *req, char *resp, size_t resplen) {
    if (g_verbose) fprintf(stderr, "--> %s\n", req);

    size_t reqlen = strlen(req);
    ssize_t sent = send(fd, req, reqlen, 0);
    if (sent < 0) { perror("send"); return -1; }

    /* Read until newline (JSON-RPC response is one line) */
    size_t pos = 0;
    while (pos + 1 < resplen) {
        ssize_t n = recv(fd, resp + pos, 1, 0);
        if (n <= 0) break;
        if (resp[pos] == '\n') { resp[pos] = '\0'; break; }
        pos++;
    }
    resp[pos < resplen ? pos : resplen - 1] = '\0';

    if (g_verbose) fprintf(stderr, "<-- %s\n", resp);
    return 0;
}

/* Extract "result" value from JSON-RPC response for printing */
static void print_result(const char *resp) {
    const char *r = strstr(resp, "\"result\":");
    if (!r) {
        /* Check for error */
        const char *e = strstr(resp, "\"error\":");
        if (e) {
            const char *msg = strstr(e, "\"message\":\"");
            if (msg) {
                msg += 11;
                const char *end = strchr(msg, '"');
                if (end) {
                    printf("error: %.*s\n", (int)(end - msg), msg);
                    return;
                }
            }
            printf("error: %s\n", resp);
        } else {
            printf("%s\n", resp);
        }
        return;
    }
    r += 9;  /* skip "result": */
    /* Pretty-print: expand commas with newlines for readability */
    int in_str = 0;
    int depth  = 0;
    while (*r) {
        char c = *r++;
        if (!in_str) {
            if (c == '"') { in_str = 1; putchar(c); continue; }
            if (c == '{' || c == '[') {
                putchar(c); putchar('\n');
                depth++;
                for (int i = 0; i < depth * 2; i++) putchar(' ');
                continue;
            }
            if (c == '}' || c == ']') {
                putchar('\n');
                depth--;
                for (int i = 0; i < depth * 2; i++) putchar(' ');
                putchar(c);
                continue;
            }
            if (c == ',') {
                putchar(c); putchar('\n');
                for (int i = 0; i < depth * 2; i++) putchar(' ');
                continue;
            }
        } else {
            if (c == '\\') { putchar(c); if (*r) putchar(*r++); continue; }
            if (c == '"')  { in_str = 0; }
        }
        putchar(c);
    }
    putchar('\n');
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [-s socket] [-v] <command>\n"
        "\n"
        "Commands:\n"
        "  status          simulator state and capacity\n"
        "  smart           SMART/Health log page (16 fields)\n"
        "  perf            performance counters and latency percentiles\n"
        "  perf reset      reset rolling window\n"
        "  gc trigger      request GC cycle\n"
        "  trace enable    enable command trace\n"
        "  trace disable   disable command trace\n"
        "  log             recent log entries\n"
        "  config get      current configuration\n"
        "\n"
        "Options:\n"
        "  -s socket       Unix socket path (default: %s)\n"
        "  -v              verbose (show raw JSON)\n",
        prog, DEFAULT_SOCK);
}

int main(int argc, char *argv[]) {
    const char *sock_path = DEFAULT_SOCK;
    int opt;

    while ((opt = getopt(argc, argv, "s:v")) != -1) {
        switch (opt) {
        case 's': sock_path = optarg; break;
        case 'v': g_verbose = 1;     break;
        default:  usage(argv[0]); return 1;
        }
    }

    if (optind >= argc) {
        usage(argv[0]);
        return 1;
    }

    /* Build JSON-RPC method name from command words */
    char method[64] = "";
    int  i = optind;

    if (!strcmp(argv[i], "status")) {
        strncpy(method, "status.get", sizeof(method) - 1);
    } else if (!strcmp(argv[i], "smart")) {
        strncpy(method, "smart.get", sizeof(method) - 1);
    } else if (!strcmp(argv[i], "perf")) {
        if (i + 1 < argc && !strcmp(argv[i+1], "reset"))
            strncpy(method, "perf.reset", sizeof(method) - 1);
        else
            strncpy(method, "perf.get", sizeof(method) - 1);
    } else if (!strcmp(argv[i], "gc")) {
        if (i + 1 < argc && !strcmp(argv[i+1], "trigger"))
            strncpy(method, "gc.trigger", sizeof(method) - 1);
        else
            strncpy(method, "gc.trigger", sizeof(method) - 1);
    } else if (!strcmp(argv[i], "trace")) {
        if (i + 1 < argc && !strcmp(argv[i+1], "disable"))
            strncpy(method, "trace.disable", sizeof(method) - 1);
        else
            strncpy(method, "trace.enable", sizeof(method) - 1);
    } else if (!strcmp(argv[i], "log")) {
        strncpy(method, "log.get", sizeof(method) - 1);
    } else if (!strcmp(argv[i], "config")) {
        strncpy(method, "config.get", sizeof(method) - 1);
    } else {
        fprintf(stderr, "unknown command: %s\n", argv[i]);
        usage(argv[0]);
        return 1;
    }

    char req[256];
    snprintf(req, sizeof(req),
             "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":{},\"id\":1}",
             method);

    int fd = connect_sock(sock_path);
    if (fd < 0) return 1;

    char resp[BUF_SIZE];
    if (send_recv(fd, req, resp, sizeof(resp)) != 0) {
        close(fd);
        return 1;
    }

    print_result(resp);

    close(fd);
    return 0;
}
