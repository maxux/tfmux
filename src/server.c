#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <libwebsockets.h>
#include <json.h>

#include "server.h"
#include "utils.h"

#ifndef TTYD_VERSION
#define TTYD_VERSION "unknown"
#endif

void *mainthread_run_command(void *args);

volatile bool force_exit = false;
struct lws_context *context;
struct tty_server *server;

int __verbose = 0;

char *__process_states[] = {"created", "starting", "running", "stopping", "stopped", "crashed"};

// websocket protocols
static const struct lws_protocols protocols[] = {
        {"http-only", callback_http, sizeof(struct pss_http),   0},
        {"tty",       callback_tty,  sizeof(struct tty_client), 0},
        {NULL, NULL, 0, 0}
};

// websocket extensions
static const struct lws_extension extensions[] = {
        {"permessage-deflate", lws_extension_callback_pm_deflate, "permessage-deflate"},
        {"deflate-frame",      lws_extension_callback_pm_deflate, "deflate_frame"},
        {NULL, NULL, NULL}
};

// command line options
static const struct option options[] = {
        {"port",         required_argument, NULL, 'p'},
        {"interface",    required_argument, NULL, 'i'},
        {"credential",   required_argument, NULL, 'c'},
        {"uid",          required_argument, NULL, 'u'},
        {"gid",          required_argument, NULL, 'g'},
        {"signal",       required_argument, NULL, 's'},
        {"signal-list",  no_argument,       NULL, '1'},
        {"reconnect",    required_argument, NULL, 'r'},
        {"index",        required_argument, NULL, 'I'},
        {"ipv6",         no_argument,       NULL, '6'},
        {"ssl",          no_argument,       NULL, 'S'},
        {"ssl-cert",     required_argument, NULL, 'C'},
        {"ssl-key",      required_argument, NULL, 'K'},
        {"ssl-ca",       required_argument, NULL, 'A'},
        {"readonly",     no_argument,       NULL, 'R'},
        {"check-origin", no_argument,       NULL, 'O'},
        {"max-clients",  required_argument, NULL, 'm'},
        {"once",         no_argument,       NULL, 'o'},
        {"debug",        required_argument, NULL, 'd'},
        {"version",      no_argument,       NULL, 'v'},
        {"help",         no_argument,       NULL, 'h'},
        {NULL, 0, 0, 0}
};
static const char *opt_string = "p:i:c:u:g:s:r:I:6aSC:K:A:Rt:T:Om:od:vh";

void print_help() {
    fprintf(stderr, "ttyd is a tool for sharing terminal over the web\n\n"
                    "USAGE:\n"
                    "    ttyd [options] <command> [<arguments...>]\n\n"
                    "VERSION:\n"
                    "    %s\n\n"
                    "OPTIONS:\n"
                    "    -p, --port              Port to listen (default: 7681, use `0` for random port)\n"
                    "    -i, --interface         Network interface to bind (eg: eth0), or UNIX domain socket path (eg: /var/run/ttyd.sock)\n"
                    "    -c, --credential        Credential for Basic Authentication (format: username:password)\n"
                    "    -u, --uid               User id to run with\n"
                    "    -g, --gid               Group id to run with\n"
                    "    -s, --signal            Signal to send to the command when exit it (default: 1, SIGHUP)\n"
                    "    -r, --reconnect         Time to reconnect for the client in seconds (default: 10)\n"
                    "    -R, --readonly          Do not allow clients to write to the TTY\n"
                    "    -t, --client-option     Send option to client (format: key=value), repeat to add more options\n"
                    "    -O, --check-origin      Do not allow websocket connection from different origin\n"
                    "    -m, --max-clients       Maximum clients to support (default: 0, no limit)\n"
                    "    -o, --once              Accept only one client and exit on disconnection\n"
                    "    -I, --index             Custom index.html path\n"
                    "    -6, --ipv6              Enable IPv6 support\n"
                    "    -S, --ssl               Enable SSL\n"
                    "    -C, --ssl-cert          SSL certificate file path\n"
                    "    -K, --ssl-key           SSL key file path\n"
                    "    -A, --ssl-ca            SSL CA file path for client certificate verification\n"
                    "    -d, --debug             Set log level (default: 7)\n"
                    "    -v, --version           Print the version and exit\n"
                    "    -h, --help              Print this text and exit\n\n"
                    "Visit https://github.com/tsl0922/ttyd to get more information and report bugs.\n",
            TTYD_VERSION
    );
}

void *warnp(char *str) {
    fprintf(stderr, "[-] %s: %s\n", str, strerror(errno));
    return NULL;
}

//
// generic buffer
//
buffer_t *buffer_new(size_t length) {
    buffer_t *buffer = xmalloc(sizeof(buffer_t));
    buffer->buffer = xmalloc(length);
    buffer->length = length;

    return buffer;
}

void buffer_free(buffer_t *buffer) {
    free(buffer->buffer);
    free(buffer);
}

//
// circular buffer
//
circbuf_t *circular_new(size_t length) {
    circbuf_t *circular = xmalloc(sizeof(circbuf_t));

    circular->length = length;
    circular->buffer = xmalloc(length);
    circular->reader = circular->buffer;
    circular->writer = circular->buffer;

    return circular;
}

void circular_free(circbuf_t *circular) {
    free(circular->buffer);
    circular->length = 0;

    free(circular);
}

size_t circular_append(circbuf_t *circular, uint8_t *data, size_t length) {
    // we have enough space on the buffer to write our data
    // it's easy
    if(circular->writer - circular->buffer + length < circular->length) {
        // printf("<> circular append, case 1\n");
        memcpy(circular->writer, data, length);
        circular->writer += length;

        // reset writer to the beginin if we are at the end
        // CHECK: maybe will never occures because of first if
        if(circular->writer == circular->buffer + circular->length)
            circular->writer = circular->buffer;

        return length;
    }

    // if data is larger than our circular buffer
    // let's just put the latest data available in the beginin
    if(length >= circular->length) {
        printf("<> circular append, case 2\n");
        memcpy(circular->buffer, data + length - circular->length, circular->length);
        circular->writer = circular->buffer;
        circular->reader = circular->buffer;
        return length;
    }

    // printf("<> circular append, case 3\n");

    // we don't have enough space to store data in one shot
    // let's copy what we can, then go back to the beginin and
    // write again, ...
    size_t bufremain = circular->length - (circular->writer - circular->buffer);
    memcpy(circular->writer, data, bufremain);
    circular->writer = circular->buffer;

    memcpy(circular->writer, data + bufremain, length - bufremain);
    circular->writer += length - bufremain;
}

buffer_t *circular_get(circbuf_t *circular, size_t length) {
    if(length > circular->length)
        return NULL;

    if(length == 0) {
        // buffer is full, we know length will be
        // the full buffer
        if(circular->reader != circular->buffer) {
            length = circular->length;

        } else {
            // the buffer is not full, computing
            // how long is it right now
            length = circular->writer - circular->buffer;
        }
    }

    buffer_t *response = buffer_new(length);

    if(circular->reader == circular->buffer) {
        // if the reader is on the beginin, we can
        // just copy available data and returns it
        memcpy(response->buffer, circular->reader, length);
        return response;
    }

    // buffer is full, we need to start from reader, copy
    // 'til the end of the buffer, then appends the rest which
    // is the beginin of the buffer
    size_t remain = circular->length - (circular->reader - circular->buffer);

    memcpy(response->buffer, circular->reader, remain);
    memcpy(response->buffer + remain, circular->buffer, circular->reader - circular->buffer);

    return response;
}

struct tty_server *tty_server_new() {
    struct tty_server *ts;

    ts = xmalloc(sizeof(struct tty_server));
    memset(ts, 0, sizeof(struct tty_server));

    LIST_INIT(&ts->clients);
    LIST_INIT(&ts->processes);

    ts->client_count = 0;
    ts->reconnect = 10;
    ts->sig_code = SIGHUP;

    sprintf(ts->terminal_type, "%s", "xterm-256color");
    get_sig_name(ts->sig_code, ts->sig_name, sizeof(ts->sig_name));

    return ts;
}

char *tty_server_process_state(struct tty_process *process) {
    pthread_mutex_lock(&process->mutex);
    char *state = __process_states[process->state];
    pthread_mutex_unlock(&process->mutex);

    return state;
}

struct tty_process *tty_server_process_stop(struct tty_process *process) {
    if(process->running == false)
        return NULL;

    verbose("[+] killing process: %d\n", process->pid);

    kill(process->pid, SIGTERM);

    pthread_mutex_lock(&process->mutex);
    process->running = false;
    process->state = STOPPING;
    pthread_mutex_unlock(&process->mutex);

    return process;
}

struct tty_process *tty_server_process_start(struct tty_server *ts, int argc, char **argv) {
    struct tty_process *process;
    size_t cmd_len = 0;

    process = xmalloc(sizeof(struct tty_process));
    memset(process, 0, sizeof(struct tty_process));

    // internal id is just variable's address
    // it's unique for the running instance
    process->id = (size_t) process;

    // shared memory across forks
    process->error = mmap(NULL, sizeof(char *), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    *process->error = NULL;

    process->state = CREATED;
    process->server = server;
    process->wstatus = 0;

    process->argv = xmalloc(sizeof(char *) * (argc + 1));
    for (int i = 0; i < argc; i++) {
        process->argv[i] = strdup(argv[i]);
        cmd_len += strlen(process->argv[i]);
        if (i != argc - 1) {
            cmd_len++; // for space
        }
    }
    process->argv[argc] = NULL;

    process->command = xmalloc(cmd_len + 1);
    char *ptr = process->command;
    for (int i = 0; i < argc; i++) {
        ptr = stpcpy(ptr, process->argv[i]);
        if (i != argc - 1) {
            *ptr++ = ' ';
        }
    }

    *ptr = '\0'; // null terminator

    process->logs = circular_new(LOGS_SIZE);

    // initial lock, will unlock when process is ready
    pthread_mutex_init(&process->mutex, NULL);
    pthread_cond_init(&process->notifier, NULL);

    // starting the process
    if(pthread_create(&process->thread, NULL, mainthread_run_command, process))
        return warnp("pthread_create");

    pthread_mutex_lock(&ts->mutex);
    LIST_INSERT_HEAD(&ts->processes, process, list);
    pthread_mutex_unlock(&ts->mutex);

    return process;
}

void process_remove(struct tty_process *process) {
    // cleaning shared memory
    munmap(process->error, sizeof(char *));

    pthread_join(process->thread, NULL);

    for(int i = 0; ; i++) {
        if(process->argv[i] == NULL)
            break;

        free(process->argv[i]);
    }

    free(process->argv);
    free(process->command);

    circular_free(process->logs);

    pthread_mutex_lock(&server->mutex);
    LIST_REMOVE(process, list);
    pthread_mutex_unlock(&server->mutex);

    free(process);
}

struct tty_process *process_getby_pid(int pid, int only_running) {
    struct tty_process *process;
    struct tty_process *found = NULL;

    pthread_mutex_lock(&server->mutex);

    LIST_FOREACH(process, &server->processes, list) {
        if(process->running == false && only_running == 1)
            continue;

        if(process->pid == pid) {
            found = process;
            break;
        }
    }

    pthread_mutex_unlock(&server->mutex);

    return found;
}

struct tty_process *process_getby_id(size_t id) {
    struct tty_process *process;
    struct tty_process *found = NULL;

    pthread_mutex_lock(&server->mutex);

    LIST_FOREACH(process, &server->processes, list) {
        if(process->id == id) {
            found = process;
            break;
        }
    }

    pthread_mutex_unlock(&server->mutex);

    return found;
}


void tty_server_free(struct tty_server *ts) {
    if (ts == NULL)
        return;

    if (ts->credential != NULL)
        free(ts->credential);

    if (ts->index != NULL)
        free(ts->index);

    // free(ts->command);
    free(ts->prefs_json);
    int i = 0;

    /*
    do {
        free(ts->argv[i++]);

    } while (ts->argv[i] != NULL);

    free(ts->argv);
    */

    if (strlen(ts->socket_path) > 0) {
        struct stat st;
        if (!stat(ts->socket_path, &st)) {
            unlink(ts->socket_path);
        }
    }

    pthread_mutex_destroy(&ts->mutex);
    free(ts);
}

void sig_handler(int sig) {
    if (force_exit)
        exit(EXIT_FAILURE);

    char sig_name[20];
    get_sig_name(sig, sig_name, sizeof(sig_name));
    verbose("[+] received signal: %s (%d), exiting...\n", sig_name, sig);
    force_exit = true;

    // killing processes
    struct tty_process *process;

    pthread_mutex_lock(&server->mutex);

    LIST_FOREACH(process, &server->processes, list) {
        tty_server_process_stop(process);
        // FIXME: defunct
    }

    pthread_mutex_unlock(&server->mutex);

    lws_cancel_service(context);
    verbose("[+] waiting, you can force with another SIGINT\n");
}

int main(int argc, char **argv) {
    int __argc = 1;
    char *__argv[1] = {"/bin/bash"};

    server = tty_server_new();

    tty_server_process_start(server, __argc, __argv);

    int __nargc = 5;
    char *__nargv[5] = {"/usr/bin/python4", "/tmp/maxux-ttyd.py", "--demo", "--argument", "debug"};
    tty_server_process_start(server, __nargc, __nargv);

    pthread_mutex_init(&server->mutex, NULL);

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = 7681;
    info.iface = NULL;
    info.protocols = protocols;
    info.ssl_cert_filepath = NULL;
    info.ssl_private_key_filepath = NULL;
    info.gid = -1;
    info.uid = -1;
    info.max_http_header_pool = 16;
    info.options = LWS_SERVER_OPTION_VALIDATE_UTF8 | LWS_SERVER_OPTION_DISABLE_IPV6;
    info.extensions = extensions;

    int debug_level = LLL_ERR | LLL_WARN | LLL_NOTICE;
    char iface[128] = "";
    bool ssl = false;
    char cert_path[1024] = "";
    char key_path[1024] = "";
    char ca_path[1024] = "";

    struct json_object *client_prefs = json_object_new_object();

    // parse command line options
    int c;
    while ((c = getopt_long(argc, argv, opt_string, options, NULL)) != -1) {
        switch (c) {
            case 'h':
                print_help();
                return 0;
            case 'v':
                printf("ttyd version %s\n", TTYD_VERSION);
                return 0;
            case 'd':
                debug_level = atoi(optarg);
                break;
            case 'R':
                server->readonly = true;
                break;
            case 'O':
                server->check_origin = true;
                break;
            case 'm':
                server->max_clients = atoi(optarg);
                break;
            case 'o':
                server->once = true;
                break;
            case 'p':
                info.port = atoi(optarg);
                if (info.port < 0) {
                    fprintf(stderr, "ttyd: invalid port: %s\n", optarg);
                    return -1;
                }
                break;
            case 'i':
                strncpy(iface, optarg, sizeof(iface) - 1);
                iface[sizeof(iface) - 1] = '\0';
                break;
            case 'c':
                if (strchr(optarg, ':') == NULL) {
                    fprintf(stderr, "ttyd: invalid credential, format: username:password\n");
                    return -1;
                }
                server->credential = base64_encode((const unsigned char *) optarg, strlen(optarg));
                break;
            case 'u':
                info.uid = atoi(optarg);
                break;
            case 'g':
                info.gid = atoi(optarg);
                break;
            case 's': {
                int sig = get_sig(optarg);
                if (sig > 0) {
                    server->sig_code = sig;
                    get_sig_name(sig, server->sig_name, sizeof(server->sig_name));
                } else {
                    fprintf(stderr, "ttyd: invalid signal: %s\n", optarg);
                    return -1;
                }
            }
                break;
            case 'r':
                server->reconnect = atoi(optarg);
                if (server->reconnect <= 0) {
                    fprintf(stderr, "ttyd: invalid reconnect: %s\n", optarg);
                    return -1;
                }
                break;
            case 'I':
                if (!strncmp(optarg, "~/", 2)) {
                    const char* home = getenv("HOME");
                    server->index = malloc(strlen(home) + strlen(optarg) - 1);
                    sprintf(server->index, "%s%s", home, optarg + 1);
                } else {
                    server->index = strdup(optarg);
                }
                struct stat st;
                if (stat(server->index, &st) == -1) {
                    fprintf(stderr, "Can not stat index.html: %s, error: %s\n", server->index, strerror(errno));
                    return -1;
                }
                if (S_ISDIR(st.st_mode)) {
                    fprintf(stderr, "Invalid index.html path: %s, is it a dir?\n", server->index);
                    return -1;
                }
                break;
            case '6':
                info.options &= ~(LWS_SERVER_OPTION_DISABLE_IPV6);
                break;
            case 'S':
                ssl = true;
                break;
            case 'C':
                strncpy(cert_path, optarg, sizeof(cert_path) - 1);
                cert_path[sizeof(cert_path) - 1] = '\0';
                break;
            case 'K':
                strncpy(key_path, optarg, sizeof(key_path) - 1);
                key_path[sizeof(key_path) - 1] = '\0';
                break;
            case 'A':
                strncpy(ca_path, optarg, sizeof(ca_path) - 1);
                ca_path[sizeof(ca_path) - 1] = '\0';
                break;
            case 'T':
                strncpy(server->terminal_type, optarg, sizeof(server->terminal_type) - 1);
                server->terminal_type[sizeof(server->terminal_type) - 1] = '\0';
                break;
            case '?':
                break;
            case 't':
                optind--;
                /*
                for (; optind < argc && *argv[optind] != '-'; optind++) {
                    char *option = strdup(optarg);
                    char *key = strsep(&option, "=");
                    if (key == NULL) {
                        fprintf(stderr, "ttyd: invalid client option: %s, format: key=value\n", optarg);
                        return -1;
                    }
                    char *value = strsep(&option, "=");
                    free(option);
                    struct json_object *obj = json_tokener_parse(value);
                    json_object_object_add(client_prefs, key, obj != NULL ? obj : json_object_new_string(value));
                }
                */
                break;
            default:
                print_help();
                return -1;
        }
    }
    server->prefs_json = strdup(json_object_to_json_string(client_prefs));
    json_object_put(client_prefs);

    /*
    if (server->command == NULL || strlen(server->command) == 0) {
        fprintf(stderr, "ttyd: missing start command\n");
        return -1;
    }
    */

    lws_set_log_level(0, NULL);

#if LWS_LIBRARY_VERSION_MAJOR >= 2
    char server_hdr[128] = "";
    sprintf(server_hdr, "ttyd/%s (libwebsockets/%s)", TTYD_VERSION, LWS_LIBRARY_VERSION);
    info.server_string = server_hdr;
#if LWS_LIBRARY_VERSION_MINOR >= 1
    info.ws_ping_pong_interval = 5;
#endif
#endif

    if (strlen(iface) > 0) {
        info.iface = iface;
        if (endswith(info.iface, ".sock") || endswith(info.iface, ".socket")) {
#if defined(LWS_USE_UNIX_SOCK) || defined(LWS_WITH_UNIX_SOCK)
            info.options |= LWS_SERVER_OPTION_UNIX_SOCK;
            strncpy(server->socket_path, info.iface, sizeof(server->socket_path));
#else
            fprintf(stderr, "libwebsockets is not compiled with UNIX domain socket support");
            return -1;
#endif
        }
    }

    if (ssl) {
        info.ssl_cert_filepath = cert_path;
        info.ssl_private_key_filepath = key_path;
        info.ssl_ca_filepath = ca_path;
        info.ssl_cipher_list = "ECDHE-ECDSA-AES256-GCM-SHA384:"
                "ECDHE-RSA-AES256-GCM-SHA384:"
                "DHE-RSA-AES256-GCM-SHA384:"
                "ECDHE-RSA-AES256-SHA384:"
                "HIGH:!aNULL:!eNULL:!EXPORT:"
                "!DES:!MD5:!PSK:!RC4:!HMAC_SHA1:"
                "!SHA1:!DHE-RSA-AES128-GCM-SHA256:"
                "!DHE-RSA-AES128-SHA256:"
                "!AES128-GCM-SHA256:"
                "!AES128-SHA256:"
                "!DHE-RSA-AES256-SHA256:"
                "!AES256-GCM-SHA384:"
                "!AES256-SHA256";
        if (strlen(info.ssl_ca_filepath) > 0)
            info.options |= LWS_SERVER_OPTION_REQUIRE_VALID_OPENSSL_CLIENT_CERT;
#if LWS_LIBRARY_VERSION_MAJOR >= 2
        info.options |= LWS_SERVER_OPTION_REDIRECT_HTTP_TO_HTTPS;
#endif
    }

    verbose("[+] initializing tfmux %s (libwebsockets %s)\n", TTYD_VERSION, LWS_LIBRARY_VERSION);
    verbose("[+] tty configuration:\n");

    if(server->credential != NULL)
        verbose("[+]   credential: %s\n", server->credential);

    verbose("[+]   close signal: %s (%d)\n", server->sig_name, server->sig_code);
    verbose("[+]   terminal type: %s\n", server->terminal_type);
    verbose("[+]   reconnect timeout: %ds\n", server->reconnect);

    if(server->check_origin)
        verbose("[+]   check origin: true\n");

    if(server->readonly)
        verbose("[+]   readonly: true\n");

    if(server->max_clients > 0)
        verbose("[+]   max clients: %d\n", server->max_clients);

    if(server->index != NULL)
        verbose("[+]   custom index.html: %s\n", server->index);

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    context = lws_create_context(&info);
    if(context == NULL) {
        fprintf(stderr, "[-] libwebsockets init failed\n");
        return 1;
    }

    // libwebsockets main loop
    while(!force_exit) {
        lws_service(context, 10);
    }

    lws_context_destroy(context);

    // cleanup
    tty_server_free(server);

    return 0;
}
