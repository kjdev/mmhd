#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <unistd.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>

#include <microhttpd.h>

#include <syslog.h>
#include <signal.h>

static int interrupted = 0;
static int verbose = 0;

#define _err(...) fprintf(stderr, "ERR: "__VA_ARGS__)
#define _debug(l, ...) if (verbose >= l) { printf(__VA_ARGS__); }

#include "sundown/markdown.h"
#include "sundown/html.h"
#include "sundown/buffer.h"

#define SUNDOWN_READ_UNIT   1024
#define SUNDOWN_OUTPUT_UNIT 64

#define BLOCK_SIZE 32768  /* 32k page size */

#define DEFAULT_PORT 8888
#define DEFAULT_ROOTDIR "."
#define DEFAULT_DIRECTORY_INDEX "index.md"
#define DEFAULT_STYLE "<!DOCTYPE html><html><head><meta http-equiv=\"Content-Type\" content=\"text/html;charset=UTF-8\"/><title>Markdown</title><style type=\"text/css\"><!--h1{font-size:28px;color:rgb(0, 0, 0);}h2{font-size:24px;border-bottom:1px solid rgb(204,204,204);color:rgb(0,0,0);maargin:20px 0pt 10px;padding:0pt;font-wight:bold;}ul,ol{padding-left:30px;}strong,b{font-weight:bold;}table{border-collapse:collapse;border-spacing:0;font:inherit;margin:auto;}table td{border-bottom:1px solid #ddd;}table th{font-weight:bold;}table th,table td{border:1px solid rgb(204,204,204);padding:6px 13px;}table tr{border-top:1px solid #ccc;background-color:#fff}img{display:block;margin:auto;}video{display:block;margin:auto;}pre{background-color:#f8f8f8;border:1px solid #ddd;border-radius:3px 3px 3px 3px;font-size:13px;line-height:19px;overflow:auto;padding:6px 10px;}pre code,pre tt{background-color:transparent;border:medium none;margin:0;padding:0;}pre>code{white-space:pre;}code{white-space:nowrap;}code,tt{background-color:#f8f8f8;border:1px solid #ddd;border-radius:3px 3px 3px 3px;margin:0 2px;padding:0 5px;}.toc{padding:1em 1.5em;color:#999;font-size:.75em;margin-bottom:3em;border:1px solid #999;float:right;list-style-position:inside;background:#fff;}.toc li{}.toc>li{margin-bottom:1em;}.toc a,.toc a:link,.toc a:visited{color:#666;text-decoration:none;}.toc a:hover{color:#999;text-decoration:underline;}.toc>li>a{font-weight:bold;}.toc li li{}.toc li li a{}--></style></head><body></body></html>"
#define DEFAULT_PIDFILE "/tmp/mmhd.pid"

typedef struct {
    char *root_dir;
    char *directory_index;
    char *style_file;
    int enable_toc;
} response_params_t;

struct mime_type_t {
    int markdown;
    const char *ext;
    const char *type;
};

static const struct mime_type_t mimetype[] = {
    { 1, ".md", "text/html; charset=UTF-8" },
    { 1, ".markdown", "text/html; charset=UTF-8" },
    { 0, ".html", "text/html; charset=UTF-8" },
    { 0, ".htm", "text/html; charset=UTF-8" },
    { 0, ".png", "image/png" },
    { 0, ".jpg", "image/jpeg" },
    { 0, ".jpeg", "image/jpeg" },
    { 0, ".gif", "image/gif" },
    { 0, ".ico", "image/x-ico" },
    { 0, ".css", "text/css" },
    { 0, ".js", "application/javascript" },
    { 0, ".txt", "text/plain" }
};

static char *
contents_generate(size_t *length,
                  const char *data, const size_t data_size,
                  const char *toc, const size_t toc_size,
                  const char *style_file)
{
    char *retval = NULL;
    size_t pos = 0;
    char *style = NULL;
    size_t style_size = 0;
    int style_free = 0;
    struct stat statbuf;

    *length = 0;

    if (style_file && stat(style_file, &statbuf) == 0) {
        FILE *file = fopen(style_file, "rb");
        if (file) {
            style_size = statbuf.st_size;
            style = (char *)malloc(sizeof(char) * (style_size + 1));
            if (style) {
                fread(style, 1, style_size, file);
                style_free = 1;
            }
            fclose(file);
        }
    }
    if (!style) {
        style = DEFAULT_STYLE;
        style_size = strlen(style);
    }

    retval = (char *)malloc(sizeof(char) * (data_size+toc_size+style_size+1));
    if (!retval) {
        if (style_free) {
            free(style);
        }
        return NULL;
    }

    if (style && style_size) {
        char *str = strstr(style, "</body>");
        if (str) {
            pos = str - style;
            memcpy(retval, style, pos);
            *length += pos;
        } else {
            memcpy(retval, style, style_size);
            *length += style_size;
        }
    }

    if (toc && toc_size) {
        memcpy(retval + *length, toc, toc_size);
        *length += toc_size;
    }

    if (data && data_size) {
        memcpy(retval + *length, data, data_size);
        *length += data_size;
    }

    if (pos) {
        memcpy(retval + *length, style + pos, style_size - pos);
        *length += (style_size - pos);
    }

    if (style_free) {
        free(style);
    }

    return retval;
}

static ssize_t
file_output_cb(void *cls, uint64_t pos, char *buf, size_t max)
{
    FILE *file = cls;

    (void)fseek(file, pos, SEEK_SET);

    return fread(buf, 1, max, file);
}

static void
file_destroy_cb(void *cls)
{
    FILE *file = cls;
    fclose(file);
}

static int
response_cb(void *cls, struct MHD_Connection *connection, const char *url,
            const char *method, const char *version, const char *upload_data,
            size_t *upload_data_size, void **ptr)
{
    response_params_t *params = (response_params_t *)cls;
    char filepath[PATH_MAX+1] = {0,};
    static int aptr;
    struct MHD_Response *response;
    int ret;
    FILE *file;
    struct stat statbuf;
    char *ext = NULL;
    char *content = NULL;
    size_t content_length = 0;
    const char *content_type = "text/plain";

    if (strcmp(method, "GET") != 0) {
        /* unexpected method */
        return MHD_NO;
    }

    if (&aptr != *ptr) {
        /* do never respond on first call */
        *ptr = &aptr;
        return MHD_YES;
    }
    *ptr = NULL; /* reset when done */

    _debug(1, "URL=[%s]\n", url);

    snprintf(filepath, PATH_MAX, "%s%s", params->root_dir, url);
    if (stat(filepath, &statbuf) == 0) {
        if (S_ISDIR(statbuf.st_mode)) {
            snprintf(filepath, PATH_MAX, "%s%s%s",
                     params->root_dir, url, params->directory_index);
            if (stat(filepath, &statbuf) == 0) {
                file = fopen(filepath, "rb");
            } else {
                file = NULL;
            }
        } else {
            file = fopen(filepath, "rb");
        }
        if (file) {
            _debug(2, "FilePath=[%s]\n", filepath);
            ext = strrchr(filepath, '.');
            if (!ext || ext == filepath) {
                ext = NULL;
            }
        }
    } else {
        file = NULL;
    }

    if (file == NULL) {
        content = contents_generate(&content_length,
                                    "File not found", 14,
                                    NULL, 0,
                                    params->style_file);
        if (content == NULL) {
            return MHD_NO;
        }

        response = MHD_create_response_from_buffer(content_length, content,
                                                   MHD_RESPMEM_MUST_FREE);
        if (response == NULL) {
            free(content);
            return MHD_NO;
        }

        content_type = "text/html; charset=UTF-8";
    } else if (ext) {
        int i;
        const char *raw = NULL, *toc = NULL;

        for (i = 0; i < (int)(sizeof(mimetype)/sizeof(mimetype[0])-1); i++) {
            if (strcasecmp(ext, mimetype[i].ext) == 0) {
                break;
            }
        }
        content_type = mimetype[i].type;

        raw = MHD_lookup_connection_value(connection,
                                          MHD_GET_ARGUMENT_KIND, "raw");
        toc = MHD_lookup_connection_value(connection,
                                          MHD_GET_ARGUMENT_KIND, "toc");

        if (raw != NULL) {
            content_type = "text/plain";
        } else if (mimetype[i].markdown) {
            int markdown_extensions = 0;
            int toc_begin = 2, toc_end = 0;
            struct buf *ib, *ob, *toc_ob = NULL;
            struct sd_callbacks callbacks;
            struct html_renderopt options;
            struct sd_markdown *markdown;
            int read;

            ib = bufnew(SUNDOWN_READ_UNIT);
            bufgrow(ib, SUNDOWN_READ_UNIT);
            while ((read = fread(ib->data + ib->size, 1,
                                 ib->asize - ib->size, file)) > 0) {
                ib->size += read;
                bufgrow(ib, ib->size + SUNDOWN_READ_UNIT);
            }

            fclose(file);
            file = NULL;

            markdown_extensions |= MKDEXT_FENCED_CODE;
            markdown_extensions |= MKDEXT_TABLES;
            markdown_extensions |= MKDEXT_SPECIAL_ATTRIBUTES;

            /* toc */
            if (params->enable_toc) {
                if (toc) {
                    size_t len = strlen(toc);
                    int n;

                    if (len > 0) {
                        char *delim, *toc_b = NULL, *toc_e = NULL;
                        delim = strstr(toc, ":");
                        if (delim) {
                            int i = delim - toc;
                            toc_b = strndup(toc, i++);
                            if (toc_b) {
                                n = atoi(toc_b);
                                if (n) {
                                    toc_begin = n;
                                }
                                free(toc_b);
                            }

                            toc_e = strndup(toc + i, len - i);
                            if (toc_e) {
                                n = atoi(toc_e);
                                if (n) {
                                    toc_end = n;
                                }
                                free(toc_e);
                            }
                        } else {
                            n = atoi(toc);
                            if (n) {
                                toc_begin = n;
                            }
                        }
                    }
                }

                toc_ob = bufnew(SUNDOWN_OUTPUT_UNIT);

                sdhtml_toc_renderer(&callbacks, &options);

                options.toc_data.begin_level = toc_begin;
                if (toc_end) {
                    options.toc_data.end_level = toc_end;
                }

                markdown = sd_markdown_new(markdown_extensions, 16,
                                           &callbacks, &options);

                sd_markdown_render(toc_ob, ib->data, ib->size, markdown);
                sd_markdown_free(markdown);
            }

            /* contents */
            ob = bufnew(SUNDOWN_OUTPUT_UNIT);

            sdhtml_renderer(&callbacks, &options, 0);

            options.flags = HTML_USE_XHTML | HTML_SKIP_LINEBREAK;
            options.flags |= HTML_TOC;

            markdown = sd_markdown_new(markdown_extensions, 16,
                                       &callbacks, &options);

            sd_markdown_render(ob, ib->data, ib->size, markdown);
            sd_markdown_free(markdown);

            if (toc_ob) {
                content = contents_generate(&content_length,
                                            ob->data, ob->size,
                                            toc_ob->data, toc_ob->size,
                                            params->style_file);
                bufrelease(toc_ob);
            } else {
                content = contents_generate(&content_length,
                                            ob->data, ob->size,
                                            NULL, 0,
                                            params->style_file);
            }

            bufrelease(ob);
            bufrelease(ib);

            if (content == NULL) {
                return MHD_NO;
            }

            response = MHD_create_response_from_buffer(content_length, content,
                                                       MHD_RESPMEM_MUST_FREE);
            if (response == NULL) {
                free(content);
                return MHD_NO;
            }
        }

        if (file) {
            response = MHD_create_response_from_callback(statbuf.st_size,
                                                         BLOCK_SIZE,
                                                         &file_output_cb,
                                                         file,
                                                         &file_destroy_cb);
            if (response == NULL) {
                fclose(file);
                return MHD_NO;
            }
        }
    }

    _debug(2, "ContentType=[%s]\n", content_type);

    MHD_add_response_header(response, "Content-Type", content_type);
    /* MHD_add_response_header(response, "Content-Length", len); */

    ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);

    return ret;
}

static void
signal_handler(int sig)
{
    interrupted = 1;
}

static void
signals(void)
{
    struct sigaction sa;

    sa.sa_handler = signal_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static void
usage(char *arg, char *message)
{
    char *command = arg;

    printf("Usage: %s [-p PORT] [-r DIR] [-d NAME] [-s FILE] [-t]", command);
    printf("\n%*s        [-D COMMAND] [-P FILE]", (int)strlen(command), "");
    printf("\n");

    printf("  -p, --port=PORT         server bind port [DEFAULT: %d]\n",
           DEFAULT_PORT);
    printf("  -r, --rootdir=DIR       document root directory [DEFAULT: %s]\n",
           DEFAULT_ROOTDIR);
    printf("  -d, --directory=NAME    directory index file name [DEFAULT: %s]\n",
           DEFAULT_DIRECTORY_INDEX);
    printf("  -s, --style=FILE        style file\n");
    printf("  -t, --toc               enable table of contents [DEFAULT: no]\n");

    printf("  -D, --daemonize=COMMAND daemon command [start|stop]\n");
    printf("  -P, --pidfile=FILE      daemon pid file path [DEFAULT: %s]\n",
           DEFAULT_PIDFILE);

    printf("  -v, --verbose           verbosity message\n");

    if (message) {
        printf("\nINFO: %s\n", message);
    }
}

int
main(int argc, char **argv)
{
    struct MHD_Daemon *mhd;

    int port = DEFAULT_PORT;

    struct stat statbuf;

    response_params_t params = { DEFAULT_ROOTDIR, DEFAULT_DIRECTORY_INDEX,
                                 NULL, 0 };

    char *daemonize = NULL;
    char *pidfile = DEFAULT_PIDFILE;

    int opt;
    const struct option long_options[] = {
        { "port", 1, NULL, 'p' },
        { "rootdir", 1, NULL, 'r' },
        { "directory", 1, NULL, 'd' },
        { "style", 1, NULL, 's' },
        { "toc", 0, NULL, 't' },
        { "daemonize", 1, NULL, 'D' },
        { "pidfile", 1, NULL, 'P' },
        { "verbose", 1, NULL, 'v' },
        { "help", 0, NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    while ((opt = getopt_long(argc, argv, "p:r:d:s:tD:P:vh",
                              long_options, NULL)) != -1) {
        switch (opt) {
            case 'p':
                port = atoi(optarg);
                break;
            case 'r':
                params.root_dir = optarg;
                break;
            case 'd':
                params.directory_index = optarg;
                break;
            case 's':
                params.style_file = optarg;
                break;
            case 't':
                params.enable_toc = 1;
                break;
            case 'D':
                daemonize = optarg;
                break;
            case 'P':
                pidfile = optarg;
                break;
            case 'v':
                if (optarg) {
                    verbose = atoi(optarg);
                } else {
                    verbose = 1;
                }
                break;
            default:
                usage(argv[0], NULL);
                return -1;
        }
    }

    if (port <= 0) {
        usage(argv[0], "invalid port number");
        return -1;
    }

    if (stat(params.root_dir, &statbuf) != 0) {
        fprintf(stderr, "ERR: no such document root directory: %s\n",
                params.root_dir);
        params.root_dir = ".";
    }
    _debug(2, "DocumentRoot=[%s]\n", params.root_dir);

    if (params.style_file) {
        if (stat(params.style_file, &statbuf) != 0) {
            fprintf(stderr, "ERR: no such style file: %s\n", params.style_file);
            params.style_file = NULL;
        }
    }
    _debug(2, "StyleFile=[%s]\n", params.style_file);

    /* daemonize */
    if (daemonize) {
        if (!pidfile || strlen(pidfile) <= 0) {
            usage(argv[0], "unknown pid file path");
            return -1;
        }

        _debug(2, "PidFile=[%s]\n", pidfile);

        openlog("micro markdown http server", LOG_PID, LOG_DAEMON);
        if (strcasecmp(daemonize, "start") == 0) {
            int nochdir = 1, noclose = 0;
            pid_t pid;
            FILE *file;

            if (daemon(nochdir, noclose) == -1) {
                syslog(LOG_INFO, "Failed to %s daemon\n", argv[0]);
                _err("Invalid daemon start");
                return -1;
            }
            syslog(LOG_INFO, "%s daemon startted\n", argv[0]);

            pid = getpid();
            file = fopen(pidfile, "w");
            if (file != NULL) {
                fprintf(file, "%d\n", pid);
                fclose(file);
            } else {
                syslog(LOG_INFO, "Failed to record process id to file: %d\n",
                       pid);
            }
        } else if (strcasecmp(daemonize, "stop") == 0) {
            int retval;
            pid_t pid;
            FILE *file = fopen(pidfile, "r");
            if (file != NULL) {
                fscanf(file, "%d\n", &pid);
                fclose(file);
                unlink(pidfile);
                retval = kill(pid, SIGTERM);
                if (retval == 0) {
                    syslog(LOG_INFO, "%s daemon stopped\n", argv[0]);
                }
                retval = 0;
            } else {
                retval = -1;
            }
            return retval;
        } else {
            usage(argv[0], "unknown daemon command");
            return -1;
        }
    }

    mhd = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY, port, NULL, NULL,
                           &response_cb, &params, MHD_OPTION_END);
    if (mhd == NULL) {
        return -1;
    }

    _debug(1, "Starting server [%d] ...\n", port);

    signals();
    while (!interrupted) {
        sleep(300);
    }

    _debug(1, "\nFinished\n");

    MHD_stop_daemon(mhd);

    return 0;
}
