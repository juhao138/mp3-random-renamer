/*
 * mp3rn_gui.c —— mp3rn 的图形界面（本地网页版）
 *
 * 做法：程序在 127.0.0.1 上起一个只有本机能访问的小服务，用默认浏览器当界面。
 * 不依赖任何 GUI 框架，Windows / macOS / Linux 用同一份源码编译。
 *
 * 构建
 *   zig cc -O2 -std=c11 -o mp3rn-gui.exe mp3rn_gui.c -lshell32 -lws2_32   (Windows)
 *   cc -O2 -std=c11 -o mp3rn-gui mp3rn_gui.c                               (macOS/Linux，需要 -lpthread)
 *
 * 用法
 *   mp3rn-gui                起服务并自动打开浏览器
 *   mp3rn-gui --no-browser   只起服务
 *   mp3rn-gui --port 8080    指定端口
 */

#define MP3RN_NO_MAIN 1
#include "mp3rn.c"
#include "webui_html.h"

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET sock_t;
#  define SOCK_INVALID INVALID_SOCKET
#  define sock_close closesocket
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <pthread.h>
#  include <sys/socket.h>
#  include <unistd.h>
typedef int sock_t;
#  define SOCK_INVALID (-1)
#  define sock_close close
#endif

/* ------------------------------------------------------------------ 互斥锁 */

#ifdef _WIN32
static CRITICAL_SECTION g_mtx;
#  define MUTEX_INIT() InitializeCriticalSection(&g_mtx)
#  define MUTEX_LOCK() EnterCriticalSection(&g_mtx)
#  define MUTEX_UNLOCK() LeaveCriticalSection(&g_mtx)
#else
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
#  define MUTEX_INIT() ((void)0)
#  define MUTEX_LOCK() pthread_mutex_lock(&g_mtx)
#  define MUTEX_UNLOCK() pthread_mutex_unlock(&g_mtx)
#endif

/* ------------------------------------------------------------------ 日志环 */

#define LOG_MAX 3000

typedef struct { char level; char *text; } LogLine;

static LogLine g_log[LOG_MAX];
static long g_log_total = 0;
static char g_status[256] = "就绪";

static void gui_log_sink(int level, const char *text) {
    MUTEX_LOCK();
    long idx = g_log_total % LOG_MAX;
    free(g_log[idx].text);
    g_log[idx].level = (char)(level ? 'w' : 'i');
    g_log[idx].text = xstrdup(text);
    g_log_total++;
    MUTEX_UNLOCK();
}

static void set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    MUTEX_LOCK();
    vsnprintf(g_status, sizeof g_status, fmt, ap);
    MUTEX_UNLOCK();
    va_end(ap);
}

/* ------------------------------------------------------------------ 运行状态 */

static volatile int g_busy = 0;
static volatile int g_watching = 0;
static volatile int g_stop_watch = 0;
static volatile long g_processed = 0;

/* ------------------------------------------------------------------ HTTP 小工具 */

static void send_all(sock_t s, const char *buf, size_t len) {
    while (len) {
        int n = send(s, buf, (int)(len > 65536 ? 65536 : len), 0);
        if (n <= 0) return;
        buf += n;
        len -= (size_t)n;
    }
}

static void send_response(sock_t s, int code, const char *ctype,
                          const char *body, size_t blen) {
    Buf h;
    buf_init(&h);
    buf_printf(&h, "HTTP/1.1 %d %s\r\n", code, code == 200 ? "OK" : "ERR");
    buf_printf(&h, "Content-Type: %s\r\n", ctype);
    buf_printf(&h, "Content-Length: %zu\r\n", blen);
    buf_puts(&h, "Cache-Control: no-store\r\n");
    buf_puts(&h, "Connection: close\r\n\r\n");
    send_all(s, h.data, h.len);
    if (blen) send_all(s, body, blen);
    buf_free(&h);
}

static void send_json(sock_t s, int code, const char *json) {
    send_response(s, code, "application/json; charset=utf-8", json, strlen(json));
}

static void send_err(sock_t s, const char *msg) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"ok\":false,\"error\":\"");
    json_escape(&b, msg);
    buf_puts(&b, "\"}");
    send_json(s, 400, b.data);
    buf_free(&b);
}

static int url_decode(const char *in, char *out, size_t cap) {
    size_t o = 0;
    while (*in && o + 1 < cap) {
        if (*in == '%' && isxdigit((unsigned char)in[1]) && isxdigit((unsigned char)in[2])) {
            char hex[3] = { in[1], in[2], 0 };
            out[o++] = (char)strtol(hex, NULL, 16);
            in += 3;
        } else if (*in == '+') {
            out[o++] = ' ';
            in++;
        } else {
            out[o++] = *in++;
        }
    }
    out[o] = '\0';
    return (int)o;
}

/* 从 query 串里取参数（已做 URL 解码） */
static int query_get(const char *query, const char *key, char *out, size_t cap) {
    if (!query) return 0;
    size_t klen = strlen(key);
    const char *p = query;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg > klen && p[klen] == '=' && strncmp(p, key, klen) == 0) {
            char tmp[4096];
            size_t n = seg - klen - 1;
            if (n >= sizeof tmp) n = sizeof tmp - 1;
            memcpy(tmp, p + klen + 1, n);
            tmp[n] = '\0';
            url_decode(tmp, out, cap);
            return 1;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return 0;
}

/* 从 JSON body 里取字符串字段 */
static int body_str(const char *body, const char *key, char *out, size_t cap) {
    if (!body) return 0;
    JP j = { body, 0 };
    if (!jp_expect(&j, '{')) return 0;
    jp_ws(&j);
    if (*j.p == '}') return 0;
    for (;;) {
        char *k = jp_string(&j);
        if (!k) return 0;
        if (!jp_expect(&j, ':')) { free(k); return 0; }
        int hit = strcmp(k, key) == 0;
        free(k);
        jp_ws(&j);
        if (*j.p == '"') {
            char *v = jp_string(&j);
            if (!v) return 0;
            if (hit) { snprintf(out, cap, "%s", v); free(v); return 1; }
            free(v);
        } else if (*j.p == '[' || *j.p == '{') {
            if (!jp_skip_value(&j)) return 0;
        } else {
            const char *start = j.p;
            while (*j.p && *j.p != ',' && *j.p != '}') j.p++;
            if (hit) {
                size_t n = (size_t)(j.p - start);
                while (n && (start[n-1] == ' ' || start[n-1] == '\t')) n--;
                if (n >= cap) n = cap - 1;
                memcpy(out, start, n);
                out[n] = '\0';
                return 1;
            }
        }
        jp_ws(&j);
        if (*j.p == ',') { j.p++; continue; }
        if (*j.p == '}') return 0;
        return 0;
    }
}

static int body_int(const char *body, const char *key, int def) {
    char tmp[64];
    if (!body_str(body, key, tmp, sizeof tmp)) return def;
    while (*tmp == ' ') memmove(tmp, tmp + 1, strlen(tmp));
    return atoi(tmp);
}

/* ------------------------------------------------------------------ 业务操作 */

static char *state_file_for(const char *root) { return path_join(root, STATE_NAME); }

static int do_run(const char *root, int replace, const char *sep, int digits,
                  int recursive, int dry, int reprocess, long *out_count) {
    char *sp = state_file_for(root);
    State st;
    state_init(&st, sp);
    free(sp);
    if (state_load(&st) != 0) logwarn("状态文件内容异常，当作空状态继续");

    Ctx c;
    ctx_init(&c, &st, sep, digits, replace, recursive, dry, reprocess);
    scan_dir(&c, root);
    if (!dry && st.dirty) state_save(&st);
    *out_count = c.count;
    ctx_free(&c);
    state_free(&st);
    return 0;
}

static long do_restore(const char *root, const char *sep, int recursive, int dry, int force) {
    char *sp = state_file_for(root);
    State st;
    state_init(&st, sp);
    free(sp);
    if (state_load(&st) != 0) logwarn("状态文件内容异常，当作空状态继续");

    Ctx c;
    ctx_init(&c, &st, sep, 9, 0, recursive, dry, 0);
    SkipItem *skips = NULL;
    size_t nskips = 0;
    int changed = restore_dir(&c, root, force, &skips, &nskips);
    if (nskips) {
        logwarn("有 %zu 个文件没能恢复：", nskips);
        for (size_t i = 0; i < nskips && i < 5; i++)
            logwarn("    %s —— %s", skips[i].name, skips[i].why);
        for (size_t i = 0; i < nskips; i++) { free(skips[i].name); free(skips[i].why); }
        free(skips);
    }
    if (!dry && st.dirty) state_save(&st);
    ctx_free(&c);
    state_free(&st);
    return changed;
}

/* ------------------------------------------------------------------ 监测线程 */

typedef struct {
    char *root;
    char *sep;
    int digits;
    int replace;
    int recursive;
    int dry;
    int reprocess;
    int interval_ms;
} WatchArgs;

static int watch_main(WatchArgs *a) {
    char *sp = state_file_for(a->root);
    State st;
    state_init(&st, sp);
    free(sp);
    if (state_load(&st) != 0) logwarn("状态文件内容异常，当作空状态继续");

    Ctx c;
    ctx_init(&c, &st, a->sep, a->digits, a->replace, a->recursive, a->dry, a->reprocess);

    g_watching = 1;
    set_status("监测中…… 等待 mp3 文件");
    while (!g_stop_watch) {
        int before = c.count;
        scan_dir(&c, a->root);
        if (!a->dry && st.dirty) state_save(&st);
        g_processed += c.count - before;
        if (c.count - before > 0) set_status("监测中…… 已处理 %ld 个文件", g_processed);

        int slept = 0;
        while (!g_stop_watch && slept < a->interval_ms) { sleep_ms(100); slept += 100; }
    }
    g_watching = 0;
    set_status("已停止。本次共处理 %ld 个文件。", g_processed);
    loginfo("结束，共处理 %ld 个文件。", g_processed);

    ctx_free(&c);
    state_free(&st);
    free(a->root); free(a->sep); free(a);
    return 0;
}

#ifdef _WIN32
static DWORD WINAPI watch_thread(LPVOID arg) { return (DWORD)watch_main((WatchArgs *)arg); }
static int start_watch_thread(WatchArgs *a, void **handle) {
    HANDLE h = CreateThread(NULL, 0, watch_thread, a, 0, NULL);
    if (!h) return -1;
    *handle = (void *)h;
    return 0;
}
static void join_watch(void *handle) { if (handle) { WaitForSingleObject((HANDLE)handle, 5000); CloseHandle((HANDLE)handle); } }
#else
static void *watch_thread(void *arg) { watch_main((WatchArgs *)arg); return NULL; }
static int start_watch_thread(WatchArgs *a, void **handle) {
    pthread_t t;
    if (pthread_create(&t, NULL, watch_thread, a) != 0) return -1;
    *handle = (void *)(uintptr_t)t;
    return 0;
}
static void join_watch(void *handle) { if (handle) pthread_join((pthread_t)(uintptr_t)handle, NULL); }
#endif

static void *g_watch_handle = NULL;

/* ------------------------------------------------------------------ 各接口 */

static void api_status(sock_t s) {
    Buf b;
    buf_init(&b);
    MUTEX_LOCK();
    buf_printf(&b, "{\"ok\":true,\"running\":%s,\"busy\":%s,\"processed\":%ld,\"text\":\"",
               g_watching ? "true" : "false", g_busy ? "true" : "false", g_processed);
    json_escape(&b, g_status);
    MUTEX_UNLOCK();
    buf_puts(&b, "\"}");
    send_json(s, 200, b.data);
    buf_free(&b);
}

static void api_log(sock_t s, const char *query) {
    char tmp[32];
    long since = 0;
    if (query_get(query, "since", tmp, sizeof tmp)) since = atol(tmp);

    Buf b;
    buf_init(&b);
    MUTEX_LOCK();
    long start = g_log_total > LOG_MAX ? g_log_total - LOG_MAX : 0;
    if (since < start) since = start;
    if (since > g_log_total) since = g_log_total;
    buf_puts(&b, "{\"ok\":true,\"next\":");
    buf_printf(&b, "%ld,\"lines\":[", g_log_total);
    int first = 1;
    for (long i = since; i < g_log_total; i++) {
        LogLine *l = &g_log[i % LOG_MAX];
        if (!l->text) continue;
        if (!first) buf_putc(&b, ',');
        first = 0;
        buf_printf(&b, "{\"l\":\"%c\",\"t\":\"", l->level);
        json_escape(&b, l->text);
        buf_puts(&b, "\"}");
    }
    buf_puts(&b, "]}");
    MUTEX_UNLOCK();
    send_json(s, 200, b.data);
    buf_free(&b);
}

static void api_info(sock_t s, const char *query) {
    char path[2048];
    if (!query_get(query, "path", path, sizeof path) || !*path) { send_err(s, "缺少 path"); return; }

    int is_dir = fs_is_dir(path);
    char *sp = state_file_for(path);
    State st;
    state_init(&st, sp);
    free(sp);
    if (is_dir) state_load(&st);

    long mp3 = 0;
    if (is_dir) {
        DirEntry *e = NULL;
        size_t n = 0;
        if (fs_list(path, &e, &n) == 0) {
            for (size_t i = 0; i < n; i++) {
                size_t sl = 0;
                if (!e[i].is_dir && suffix_is_mp3(e[i].name, &sl)) mp3++;
            }
            fs_list_free(e, n);
        }
    }

    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"ok\":true,\"exists\":");
    buf_puts(&b, fs_exists(path) ? "true" : "false");
    buf_puts(&b, ",\"is_dir\":");
    buf_puts(&b, is_dir ? "true" : "false");
    buf_printf(&b, ",\"mp3\":%ld,\"used\":%zu,\"orig\":%zu,\"fps\":%zu,\"writable\":",
               mp3, st.used.n, st.orig.n, st.fps.n);
    if (is_dir) {
        char *probe = check_writable(path);
        if (probe) { buf_putc(&b, '"'); json_escape(&b, probe); buf_putc(&b, '"'); free(probe); }
        else buf_puts(&b, "null");
    } else {
        buf_puts(&b, "null");
    }
    buf_putc(&b, '}');
    send_json(s, 200, b.data);
    buf_free(&b);
    state_free(&st);
}

static void api_list(sock_t s, const char *query) {
    char path[2048] = "";
    query_get(query, "path", path, sizeof path);

    if (!*path) {
        /* 没有给路径：从用户主目录开始 */
#ifdef _WIN32
        snprintf(path, sizeof path, "%s", getenv("USERPROFILE") ? getenv("USERPROFILE") : "C:\\");
#else
        snprintf(path, sizeof path, "%s", getenv("HOME") ? getenv("HOME") : "/");
#endif
    }
    if (!fs_is_dir(path)) { send_err(s, "不是文件夹"); return; }

    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"ok\":true,\"path\":\"");
    json_escape(&b, path);
    buf_puts(&b, "\",\"parent\":\"");
    {
        char parent[2048];
        snprintf(parent, sizeof parent, "%s", path);
        size_t n = strlen(parent);
        while (n > 1 && (parent[n-1] == '/' || parent[n-1] == '\\')) parent[--n] = '\0';
#ifdef _WIN32
        if (n == 2 && parent[1] == ':') { /* C: -> C:\ */ parent[2] = '\\'; parent[3] = '\0'; }
        else
#endif
        {
            while (n > 1 && parent[n-1] != '/' && parent[n-1] != '\\') parent[--n] = '\0';
            while (n > 1 && (parent[n-1] == '/' || parent[n-1] == '\\')) parent[--n] = '\0';
#ifdef _WIN32
            if (n == 2 && parent[1] == ':') { parent[2] = '\\'; parent[3] = '\0'; }
#endif
        }
        json_escape(&b, parent);
    }
    buf_puts(&b, "\",\"dirs\":[");

    DirEntry *e = NULL;
    size_t n = 0;
    if (fs_list(path, &e, &n) == 0) {
        int first = 1;
        for (size_t i = 0; i < n && i < 2000; i++) {
            if (!e[i].is_dir) continue;
            if (e[i].name[0] == '.') continue;
            if (!first) buf_putc(&b, ',');
            first = 0;
            buf_putc(&b, '"');
            json_escape(&b, e[i].name);
            buf_putc(&b, '"');
        }
        fs_list_free(e, n);
    }
    buf_puts(&b, "]}");
    send_json(s, 200, b.data);
    buf_free(&b);
}

/* 从 body 里读通用参数 */
typedef struct {
    char path[2048];
    char sep[16];
    char mode[32];
    int digits;
    int recursive;
    int dry;
    int reprocess;
    int interval_ms;
} Params;

static int parse_params(const char *body, Params *p) {
    memset(p, 0, sizeof *p);
    if (!body_str(body, "path", p->path, sizeof p->path) || !p->path[0]) return 0;
    if (!body_str(body, "sep", p->sep, sizeof p->sep)) snprintf(p->sep, sizeof p->sep, "%s", DEFAULT_SEP);
    if (!body_str(body, "mode", p->mode, sizeof p->mode)) snprintf(p->mode, sizeof p->mode, "prefix");
    p->digits = body_int(body, "digits", DEFAULT_DIGITS);
    if (p->digits < 1 || p->digits > 18) p->digits = DEFAULT_DIGITS;
    p->recursive = body_int(body, "recursive", 0);
    p->dry = body_int(body, "dry_run", 0);
    p->reprocess = body_int(body, "reprocess", 0);
    int sec = (int)(body_int(body, "interval", 1) * 1000);
    if (sec < 200) sec = 200;
    p->interval_ms = sec;
    return 1;
}

static void api_run(sock_t s, const char *body) {
    Params p;
    if (!parse_params(body, &p)) { send_err(s, "缺少 path"); return; }
    if (!fs_is_dir(p.path)) { send_err(s, "不是文件夹"); return; }
    if (g_busy) { send_err(s, "正在执行别的操作，请先停止"); return; }

    g_busy = 1;
    set_status("正在执行……");
    long count = 0;
    do_run(p.path, strcmp(p.mode, "replace") == 0, p.sep, p.digits,
           p.recursive, p.dry, p.reprocess, &count);
    set_status(p.dry ? "试运行完成，共 %ld 个文件。" : "完成，共处理 %ld 个文件。", count);
    g_busy = 0;

    Buf b;
    buf_init(&b);
    buf_printf(&b, "{\"ok\":true,\"processed\":%ld}", count);
    send_json(s, 200, b.data);
    buf_free(&b);
}

static void api_restore(sock_t s, const char *body) {
    Params p;
    if (!parse_params(body, &p)) { send_err(s, "缺少 path"); return; }
    if (!fs_is_dir(p.path)) { send_err(s, "不是文件夹"); return; }
    if (g_busy) { send_err(s, "正在执行别的操作，请先停止"); return; }

    g_busy = 1;
    set_status("正在恢复原名……");
    long n = do_restore(p.path, p.sep, p.recursive, p.dry, 0);
    set_status("恢复完成，共 %ld 个文件。", n);
    g_busy = 0;

    Buf b;
    buf_init(&b);
    buf_printf(&b, "{\"ok\":true,\"restored\":%ld}", n);
    send_json(s, 200, b.data);
    buf_free(&b);
}

static void api_watch(sock_t s, const char *body) {
    char action[32] = "start";
    body_str(body, "action", action, sizeof action);

    if (!strcmp(action, "stop")) {
        if (!g_watching && !g_busy) { send_json(s, 200, "{\"ok\":true}"); return; }
        g_stop_watch = 1;
        join_watch(g_watch_handle);
        g_watch_handle = NULL;
        g_stop_watch = 0;
        g_busy = 0;
        send_json(s, 200, "{\"ok\":true}");
        return;
    }

    if (g_watching) { send_err(s, "已经在监测了"); return; }
    if (g_busy) { send_err(s, "正在执行别的操作，请先停止"); return; }

    Params p;
    if (!parse_params(body, &p)) { send_err(s, "缺少 path"); return; }
    if (!fs_is_dir(p.path)) { send_err(s, "不是文件夹"); return; }

    WatchArgs *a = xmalloc(sizeof *a);
    a->root = xstrdup(p.path);
    a->sep = xstrdup(p.sep);
    a->digits = p.digits;
    a->replace = strcmp(p.mode, "replace") == 0;
    a->recursive = p.recursive;
    a->dry = p.dry;
    a->reprocess = p.reprocess;
    a->interval_ms = p.interval_ms;

    g_processed = 0;
    g_stop_watch = 0;
    g_busy = 1;
    loginfo("===== 开始监测 · %s =====", p.path);
    loginfo("改名方式：%s；位数 %d；%s%s",
            a->replace ? "只保留数字" : "数字加在原名前", a->digits,
            a->recursive ? "含子文件夹" : "不含子文件夹",
            a->dry ? "；试运行" : "");
    if (start_watch_thread(a, &g_watch_handle) != 0) {
        g_busy = 0;
        free(a->root); free(a->sep); free(a);
        send_err(s, "无法创建监测线程");
        return;
    }
    send_json(s, 200, "{\"ok\":true}");
}

static void api_quit(sock_t s) {
    send_json(s, 200, "{\"ok\":true}");
    loginfo("程序退出");
    exit(0);
}

/* ------------------------------------------------------------------ 服务 */

static int ci_starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
        s++; prefix++;
    }
    return 1;
}

static void handle_client(sock_t c) {
    static char req[262144];
    size_t have = 0;

    /* 读到 header 结束 */
    for (;;) {
        if (have + 4096 >= sizeof req) return;
        int n = recv(c, req + have, (int)(sizeof req - have - 1), 0);
        if (n <= 0) return;
        have += (size_t)n;
        req[have] = '\0';
        if (strstr(req, "\r\n\r\n")) break;
    }

    char *hdr_end = strstr(req, "\r\n\r\n");
    char *body = hdr_end + 4;

    /* 按 Content-Length 补齐 body */
    size_t want = 0;
    for (char *h = req; h < hdr_end; h++) {
        if (ci_starts_with(h, "Content-Length:")) { want = (size_t)atol(h + 15); break; }
    }
    size_t got = have - (size_t)(body - req);
    while (got < want) {
        if (have + 4096 >= sizeof req) break;
        int n = recv(c, req + have, (int)(sizeof req - have - 1), 0);
        if (n <= 0) break;
        have += (size_t)n;
        got += (size_t)n;
        req[have] = '\0';
        body = strstr(req, "\r\n\r\n") + 4;
    }

    char method[8] = "", target[2048] = "";
    if (sscanf(req, "%7s %2047s", method, target) != 2) return;

    char *q = strchr(target, '?');
    char query[2048] = "";
    if (q) { *q = '\0'; snprintf(query, sizeof query, "%s", q + 1); }
    const char *path = target;

    if (!strcmp(method, "GET")) {
        if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
            send_response(c, 200, "text/html; charset=utf-8",
                          (const char *)WEBUI_HTML, WEBUI_HTML_LEN);
            return;
        }
        if (!strcmp(path, "/api/status")) { api_status(c); return; }
        if (!strcmp(path, "/api/log"))    { api_log(c, query); return; }
        if (!strcmp(path, "/api/info"))   { api_info(c, query); return; }
        if (!strcmp(path, "/api/list"))   { api_list(c, query); return; }
        send_response(c, 404, "text/plain; charset=utf-8", "not found", 9);
        return;
    }

    if (!strcmp(method, "POST")) {
        if (!strcmp(path, "/api/run"))     { api_run(c, body); return; }
        if (!strcmp(path, "/api/restore")) { api_restore(c, body); return; }
        if (!strcmp(path, "/api/watch"))   { api_watch(c, body); return; }
        if (!strcmp(path, "/api/quit"))    { api_quit(c); return; }
        send_response(c, 404, "text/plain; charset=utf-8", "not found", 9);
        return;
    }

    send_response(c, 405, "text/plain; charset=utf-8", "method not allowed", 18);
}

static int open_browser(const char *url) {
#ifdef _WIN32
    wchar_t *w = u8_to_w(url);
    HINSTANCE r = ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    free(w);
    return ((INT_PTR)r > 32) ? 0 : -1;
#elif defined(__APPLE__)
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "open '%s' >/dev/null 2>&1 &", url);
    return system(cmd);
#else
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "xdg-open '%s' >/dev/null 2>&1 &", url);
    return system(cmd);
#endif
}

int main(int argc, char **argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    build_argv_utf8();
    argc = win_argc;
    argv = win_argv;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) die("无法初始化网络");
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    MUTEX_INIT();

    int port = 0;
    int open_ui = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strncmp(argv[i], "--port=", 7)) port = atoi(argv[i] + 7);
        else if (!strcmp(argv[i], "--no-browser")) open_ui = 0;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("用法: %s [--port N] [--no-browser]\n"
                   "起一个只监听 127.0.0.1 的本地服务，用浏览器当界面。\n", argv[0]);
            return 0;
        }
    }

    setvbuf(stdout, NULL, _IOLBF, 0);   /* 输出被重定向时也能及时看到 */
    rng_seed();
    g_log_sink = gui_log_sink;

    sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == SOCK_INVALID) die("无法创建套接字");

    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7f000001);   /* 只监听 127.0.0.1 */
    addr.sin_port = htons((unsigned short)port);

    if (bind(ls, (struct sockaddr *)&addr, sizeof addr) != 0) die("端口被占用（试别的 --port）");
    if (listen(ls, 16) != 0) die("监听失败");

#ifdef _WIN32
    int alen = sizeof addr;
#else
    socklen_t alen = sizeof addr;
#endif
    if (getsockname(ls, (struct sockaddr *)&addr, &alen) != 0) die("取端口失败");
    int real_port = ntohs(addr.sin_port);

    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", real_port);

    printf("MP3 随机数字改名器（图形界面）\n");
    printf("界面地址：%s\n", url);
    printf("只监听本机，关闭窗口或按 Ctrl+C 退出。\n\n");

    if (open_ui && open_browser(url) != 0)
        printf("（自动打开浏览器失败，请手动访问上面的地址）\n");

    for (;;) {
        struct sockaddr_in peer;
#ifdef _WIN32
        int plen = sizeof peer;
#else
        socklen_t plen = sizeof peer;
#endif
        sock_t c = accept(ls, (struct sockaddr *)&peer, &plen);
        if (c == SOCK_INVALID) break;
        handle_client(c);
        sock_close(c);
    }

    sock_close(ls);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
