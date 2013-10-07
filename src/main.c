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

#include "config.h"

static int interrupted = 0;
static int msgno = 0;

#define msg_error(...) if (msgno >= 0) fprintf(stderr, "mmhd: "__VA_ARGS__)
#define msg_verbose(...) if (msgno > 0) msg_error(__VA_ARGS__)
#define msg_verbose_ex(_level, ...) if (msgno >= _level) msg_error(__VA_ARGS__)

#include "hoedown/src/markdown.h"
#include "hoedown/src/html.h"
#include "hoedown/src/buffer.h"

#define HOEDOWN_READ_UNIT   1024
#define HOEDOWN_OUTPUT_UNIT 64

#define BLOCK_SIZE 32768  /* 32k page size */

#define DEFAULT_PORT 8888
#define DEFAULT_ROOTDIR "."
#define DEFAULT_DIRECTORY_INDEX "index.md"
#define DEFAULT_STYLE "<!DOCTYPE html><html><head><meta http-equiv=\"Content-Type\" content=\"text/html;charset=UTF-8\"/><title>Markdown</title><style type=\"text/css\"><!--h1{font-size:28px;color:rgb(0, 0, 0);}h2{font-size:24px;border-bottom:1px solid rgb(204,204,204);color:rgb(0,0,0);maargin:20px 0pt 10px;padding:0pt;font-wight:bold;}ul,ol{padding-left:30px;}strong,b{font-weight:bold;}table{border-collapse:collapse;border-spacing:0;font:inherit;margin:auto;}table td{border-bottom:1px solid #ddd;}table th{font-weight:bold;}table th,table td{border:1px solid rgb(204,204,204);padding:6px 13px;}table tr{border-top:1px solid #ccc;background-color:#fff}img{display:block;margin:auto;}video{display:block;margin:auto;}pre{background-color:#f8f8f8;border:1px solid #ddd;border-radius:3px 3px 3px 3px;font-size:13px;line-height:19px;overflow:auto;padding:6px 10px;}pre code,pre tt{background-color:transparent;border:medium none;margin:0;padding:0;}pre>code{white-space:pre;}code{white-space:nowrap;}code,tt{background-color:#f8f8f8;border:1px solid #ddd;border-radius:3px 3px 3px 3px;margin:0 2px;padding:0 5px;}.toc{padding:1em 1.5em;color:#999;font-size:.75em;margin-bottom:3em;border:1px solid #999;float:right;list-style-position:inside;background:#fff;}.toc li{}.toc>li{margin-bottom:1em;}.toc a,.toc a:link,.toc a:visited{color:#666;text-decoration:none;}.toc a:hover{color:#999;text-decoration:underline;}.toc>li>a{font-weight:bold;}.toc li li{}.toc li li a{}--></style></head><body></body></html>"
#define DEFAULT_PIDFILE "/tmp/mmhd.pid"

#define HOWDOWN_TOC_STARING 2
#define HOWDOWN_TOC_NESTING 6

typedef struct {
    char *root_dir;
    char *directory_index;
    char *style_file;
    unsigned int extensions;
    unsigned int html;
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

struct markdown_opts_t {
    const char *name;
    unsigned int value;
};

static const struct markdown_opts_t markdown_opts[] = {
    { "no_intra_emphasis", HOEDOWN_EXT_NO_INTRA_EMPHASIS },
    { "tables", HOEDOWN_EXT_TABLES },
    { "fenced_code", HOEDOWN_EXT_FENCED_CODE },
    { "autolink", HOEDOWN_EXT_AUTOLINK },
    { "strikethrough", HOEDOWN_EXT_STRIKETHROUGH },
    { "underline", HOEDOWN_EXT_UNDERLINE },
    { "space_headers", HOEDOWN_EXT_SPACE_HEADERS },
    { "superscript", HOEDOWN_EXT_SUPERSCRIPT },
    { "lax_spacing",  HOEDOWN_EXT_LAX_SPACING },
    { "disable_indented_code", HOEDOWN_EXT_DISABLE_INDENTED_CODE },
    { "highlight", HOEDOWN_EXT_HIGHLIGHT },
    { "footnotes", HOEDOWN_EXT_FOOTNOTES },
    { "quote", HOEDOWN_EXT_QUOTE },
    { "special_attribute", HOEDOWN_EXT_SPECIAL_ATTRIBUTE },
    { "skip_html", HOEDOWN_HTML_SKIP_HTML },
    { "skip_style", HOEDOWN_HTML_SKIP_STYLE },
    { "skip_images", HOEDOWN_HTML_SKIP_IMAGES },
    { "skip_links", HOEDOWN_HTML_SKIP_LINKS },
    { "expand_tabs", HOEDOWN_HTML_EXPAND_TABS },
    { "safelink", HOEDOWN_HTML_SAFELINK },
    { "toc", HOEDOWN_HTML_TOC },
    { "hard_wrap", HOEDOWN_HTML_HARD_WRAP },
    { "use_xhtml", HOEDOWN_HTML_USE_XHTML },
    { "escape", HOEDOWN_HTML_ESCAPE },
    { "prettify", HOEDOWN_HTML_PRETTIFY },
    { "use_task_list", HOEDOWN_HTML_USE_TASK_LIST },
    { "skip_eol", HOEDOWN_HTML_SKIP_EOL },
    { "skip_toc_escape", HOEDOWN_HTML_SKIP_TOC_ESCAPE }
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

    msg_verbose("URL=[%s]\n", url);

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
            msg_verbose_ex(2, "FilePath=[%s]\n", filepath);
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
            unsigned int extensions = params->extensions;
            unsigned int html = params->html;
            int toc_starting = HOWDOWN_TOC_STARING;
            int toc_nesting = HOWDOWN_TOC_NESTING;
            hoedown_buffer *ib, *ob, *toc_ob = NULL;
            hoedown_callbacks callbacks;
            hoedown_html_renderopt options;
            struct hoedown_markdown *markdown;
            int read;

            ib = hoedown_buffer_new(HOEDOWN_READ_UNIT);
            hoedown_buffer_grow(ib, HOEDOWN_READ_UNIT);
            while ((read = fread(ib->data + ib->size, 1,
                                 ib->asize - ib->size, file)) > 0) {
                ib->size += read;
                hoedown_buffer_grow(ib, ib->size + HOEDOWN_READ_UNIT);
            }

            fclose(file);
            file = NULL;

            /* toc */
            if (html & HOEDOWN_HTML_TOC) {
                html |= HOEDOWN_HTML_TOC;

                if (toc) {
                    size_t len = strlen(toc);
                    int n;

                    if (len > 0) {
                        char *delim, *toc_b = NULL, *toc_e = NULL;
                        delim = strchr(toc, ',');
                        if (delim) {
                            int i = delim - toc;
                            toc_b = strndup(toc, i++);
                            if (toc_b) {
                                n = atoi(toc_b);
                                if (n) {
                                    toc_starting = n;
                                }
                                free(toc_b);
                            }

                            toc_e = strndup(toc + i, len - i);
                            if (toc_e) {
                                n = atoi(toc_e);
                                if (n) {
                                    toc_nesting = n;
                                }
                                free(toc_e);
                            }
                        } else {
                            n = atoi(toc);
                            if (n) {
                                toc_starting = n;
                            }
                        }
                    }
                }

                toc_ob = hoedown_buffer_new(HOEDOWN_OUTPUT_UNIT);

                hoedown_html_toc_renderer(&callbacks, &options, 0);

                options.flags = html;

                options.toc_data.starting_level = toc_starting;
                options.toc_data.nesting_level = toc_nesting;
                options.toc_data.header = NULL;
                options.toc_data.footer = NULL;

                markdown = hoedown_markdown_new(extensions, 16,
                                                &callbacks, &options);

                hoedown_markdown_render(toc_ob, ib->data, ib->size, markdown);
                hoedown_markdown_free(markdown);
            }

            /* contents */
            ob = hoedown_buffer_new(HOEDOWN_OUTPUT_UNIT);

            hoedown_html_renderer(&callbacks, &options, 0, 0);

            //options.flags = HOEDOWN_HTML_USE_XHTML | HOEDOWN_HTML_SKIP_EOL;
            //options.flags |= HOEDOWN_HTML_TOC;
            options.flags = html;
            options.toc_data.starting_level = toc_starting;
            options.toc_data.nesting_level = toc_nesting;

            markdown = hoedown_markdown_new(extensions, 16,
                                            &callbacks, &options);

            hoedown_markdown_render(ob, ib->data, ib->size, markdown);
            hoedown_markdown_free(markdown);

            if (toc_ob) {
                content = contents_generate(&content_length,
                                            ob->data, ob->size,
                                            toc_ob->data, toc_ob->size,
                                            params->style_file);
                hoedown_buffer_free(toc_ob);
            } else {
                content = contents_generate(&content_length,
                                            ob->data, ob->size,
                                            NULL, 0,
                                            params->style_file);
            }

            hoedown_buffer_free(ob);
            hoedown_buffer_free(ib);

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

    msg_verbose_ex(2, "ContentType=[%s]\n", content_type);

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
    int i, count = sizeof(markdown_opts) / sizeof(struct markdown_opts_t);

    printf("Usage: %s [-p PORT] [-r DIR] [-d NAME] [-s FILE]",
           " [-D COMMAND] [-P FILE]\n", command);

    printf("  -p, --port=PORT         server bind port [DEFAULT: %d]\n",
           DEFAULT_PORT);
    printf("  -r, --rootdir=DIR       document root directory [DEFAULT: %s]\n",
           DEFAULT_ROOTDIR);
    printf("  -d, --directory=NAME    directory index file name [DEFAULT: %s]\n",
           DEFAULT_DIRECTORY_INDEX);
    printf("  -s, --style=FILE        style file\n");

    printf("  -D, --daemonize=COMMAND daemon command [start|stop]\n");
    printf("  -P, --pidfile=FILE      daemon pid file path [DEFAULT: %s]\n",
           DEFAULT_PIDFILE);

    printf("  -v, --verbose           verbosity message\n");

    printf("\nExtensions:\n");
    for (i = 0; i < count; i++) {
        printf("  --%s\n", markdown_opts[i].name);
    }

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
                                 NULL, 0, 0 };

    char *daemonize = NULL;
    char *pidfile = DEFAULT_PIDFILE;

    int opt;
    const struct option long_options[] = {
        { "port", 1, NULL, 'p' },
        { "rootdir", 1, NULL, 'r' },
        { "directory", 1, NULL, 'd' },
        { "style", 1, NULL, 's' },
        { "daemonize", 1, NULL, 'D' },
        { "pidfile", 1, NULL, 'P' },
        { "verbose", 1, NULL, 'v' },
        { "help", 0, NULL, 'h' },
        { markdown_opts[0].name, 0, NULL, 'E' },
        { markdown_opts[1].name, 0, NULL, 'E' },
        { markdown_opts[2].name, 0, NULL, 'E' },
        { markdown_opts[3].name, 0, NULL, 'E' },
        { markdown_opts[4].name, 0, NULL, 'E' },
        { markdown_opts[5].name, 0, NULL, 'E' },
        { markdown_opts[6].name, 0, NULL, 'E' },
        { markdown_opts[7].name, 0, NULL, 'E' },
        { markdown_opts[8].name, 0, NULL, 'E' },
        { markdown_opts[9].name, 0, NULL, 'E' },
        { markdown_opts[10].name, 0, NULL, 'E' },
        { markdown_opts[11].name, 0, NULL, 'E' },
        { markdown_opts[12].name, 0, NULL, 'E' },
        { markdown_opts[13].name, 0, NULL, 'E' },
        { markdown_opts[14].name, 0, NULL, 'H' },
        { markdown_opts[15].name, 0, NULL, 'H' },
        { markdown_opts[16].name, 0, NULL, 'H' },
        { markdown_opts[17].name, 0, NULL, 'H' },
        { markdown_opts[18].name, 0, NULL, 'H' },
        { markdown_opts[19].name, 0, NULL, 'H' },
        { markdown_opts[20].name, 0, NULL, 'H' },
        { markdown_opts[21].name, 0, NULL, 'H' },
        { markdown_opts[22].name, 0, NULL, 'H' },
        { markdown_opts[23].name, 0, NULL, 'H' },
        { markdown_opts[24].name, 0, NULL, 'H' },
        { markdown_opts[25].name, 0, NULL, 'H' },
        { markdown_opts[26].name, 0, NULL, 'H' },
        { markdown_opts[27].name, 0, NULL, 'H' },
        { NULL, 0, NULL, 0 }
    };

    int i, opts_count = 27;

    while ((opt = getopt_long(argc, argv, "p:r:d:s:D:P:EHvqVh",
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
            case 'D':
                daemonize = optarg;
                break;
            case 'P':
                pidfile = optarg;
                break;
            case 'E':
            case 'H':
                for (i = 0; i < opts_count; i++) {
                    struct markdown_opts_t opts = markdown_opts[i];
                    if (strcmp(argv[optind-1] + 2, opts.name) == 0) {
                        if (opt == 'E') {
                            params.extensions |= opts.value;
                        } else {
                            params.html |= opts.value;
                        }
                        break;
                    }
                }
                if (i == opts_count) {
                    usage(argv[0], "Unknown options");
                    return -1;
                }
                break;
            case 'v':
                if (optarg) {
                    msgno = atoi(optarg);
                } else {
                    msgno += 1;
                }
                break;
            case 'q':
                msgno = -1;
                break;
            case 'V':
                printf("Micro http server for Markdown version: %d.%d.%d\n",
                       MMHD_VERSION_MAJOR,
                       MMHD_VERSION_MINOR,
                       MMHD_VERSION_BUILD);
                return 0;
            case 'h':
                usage(argv[0], NULL);
                return -1;
        }
    }

    if (port <= 0) {
        usage(argv[0], "invalid port number");
        return -1;
    }

    if (stat(params.root_dir, &statbuf) != 0) {
        msg_error("ERROR: No such document root directory: %s\n",
                  params.root_dir);
        params.root_dir = ".";
    }
    msg_verbose_ex(2, "DocumentRoot=[%s]\n", params.root_dir);

    if (params.style_file) {
        if (stat(params.style_file, &statbuf) != 0) {
            msg_error("ERROR: No such style file: %s\n", params.style_file);
            params.style_file = NULL;
        }
    }
    msg_verbose_ex(2, "StyleFile=[%s]\n", params.style_file);

    /* daemonize */
    if (daemonize) {
        if (!pidfile || strlen(pidfile) <= 0) {
            usage(argv[0], "unknown pid file path");
            return -1;
        }

        msg_verbose_ex(2, "PidFile=[%s]\n", pidfile);

        openlog("micro markdown http server", LOG_PID, LOG_DAEMON);
        if (strcasecmp(daemonize, "start") == 0) {
            int nochdir = 1, noclose = 0;
            pid_t pid;
            FILE *file;

            if (daemon(nochdir, noclose) == -1) {
                syslog(LOG_INFO, "Failed to %s daemon\n", argv[0]);
                msg_error("Invalid daemon start");
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

    msg_verbose("Starting server [%d] ...\n", port);

    signals();
    while (!interrupted) {
        sleep(300);
    }

    msg_verbose("\nFinished\n");

    MHD_stop_daemon(mhd);

    return 0;
}
