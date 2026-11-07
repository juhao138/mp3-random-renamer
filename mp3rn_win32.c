/*
 * mp3rn_win32.c —— mp3rn 的原生 Windows 界面（Win32 + 系统控件，无第三方库）
 *
 * 复用 mp3rn.c 的全部逻辑：改名 / 重新编号 / 恢复原名（含内容指纹）。
 *
 * 外观：激活 ComCtl32 v6 视觉样式 + 自绘扁平按钮（圆角、悬停/按下反馈），
 *       去掉分组框和 3D 边框，输入框/日志用 1px 细边框。
 *
 * 构建：
 *   zig cc -O2 -std=c11 -o mp3rn-winningui.exe mp3rn_win32.c \
 *          -lshell32 -lole32 -lcomctl32 -luuid -lgdi32 -luser32
 */

#define UNICODE 1
#define _UNICODE 1
#define MP3RN_NO_MAIN 1
#include "mp3rn.c"

#include <commctrl.h>
#include <objbase.h>
#include <richedit.h>
#include <shlobj.h>

#define WM_APP_LOG  (WM_APP + 1)
#define WM_APP_DONE (WM_APP + 2)

/* ------------------------------------------------------------------ 控件 ID */

enum {
    ID_PATH = 1000, ID_BROWSE,
    ID_LBL_OPT, ID_MODE_PREFIX, ID_MODE_REPLACE, ID_SEP, ID_LBL_SEP, ID_LBL_EXAMPLE,
    ID_LBL_DIGITS, ID_DIGITS, ID_LBL_INTERVAL, ID_INTERVAL, ID_LBL_SEC,
    ID_RECURSIVE, ID_DRY, ID_REPROCESS,
    ID_RUN, ID_WATCH, ID_STOP, ID_RESTORE, ID_CLEAR,
    ID_LOG, ID_STATUS
};

/* 配色 */
#define C_BG        RGB(246, 247, 249)
#define C_TEXT      RGB(31, 35, 41)
#define C_MUTED     RGB(107, 114, 128)
#define C_LINE      RGB(214, 217, 222)
#define C_WHITE     RGB(255, 255, 255)
#define C_BLUE      RGB(31, 111, 235)
#define C_BLUE_D    RGB(20, 92, 205)
#define C_BLUE_H    RGB(48, 126, 245)
#define C_HOVER     RGB(238, 240, 244)
#define C_PRESS     RGB(226, 229, 235)
#define C_DIS       RGB(160, 164, 172)

static HWND g_hwnd;
static HWND h_lbl_path, h_path, h_browse;
static HWND h_lbl_opt;
static HWND h_rb_prefix, h_rb_replace, h_lbl_sep, h_sep, h_lbl_example;
static HWND h_lbl_digits, h_digits, h_lbl_interval, h_interval, h_lbl_sec;
static HWND h_recursive, h_dry, h_reprocess;
static HWND h_run, h_watch, h_stop, h_restore, h_clear;
static HWND h_log, h_status;

static HFONT g_font_ui, g_font_log;
static HBRUSH g_br_bg, g_br_white;
static int g_dpi = 96;
static int g_busy = 0;
static int g_mode = 0;      /* 0=单次 1=监测 2=恢复 */
static int g_watching = 0;
static volatile int g_stop_flag = 0;
static volatile int g_processed = 0;
static volatile long g_log_total = 0;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

/* 需要画 1px 细边框的输入区域 */
static RECT g_frames[8];
static int g_nframes = 0;
static RECT g_log_rect;

/* ------------------------------------------------------------------ 悬停跟踪 */

typedef struct { HWND hwnd; WNDPROC old; int hover; } BtnEntry;
static BtnEntry g_btns[8];
static int g_nbtns = 0;

static BtnEntry *btn_find(HWND h) {
    for (int i = 0; i < g_nbtns; i++) if (g_btns[i].hwnd == h) return &g_btns[i];
    return NULL;
}

static LRESULT CALLBACK btn_subclass(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    BtnEntry *e = btn_find(h);
    if (!e) return DefWindowProcW(h, msg, wp, lp);

    switch (msg) {
    case WM_MOUSEMOVE:
        if (!e->hover) {
            e->hover = 1;
            TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(h, NULL, TRUE);
        }
        break;
    case WM_MOUSELEAVE:
        if (e->hover) { e->hover = 0; InvalidateRect(h, NULL, TRUE); }
        break;
    }
    return CallWindowProcW(e->old, h, msg, wp, lp);
}

static void track_button(HWND h) {
    if (!h || g_nbtns >= (int)(sizeof g_btns / sizeof g_btns[0])) return;
    g_btns[g_nbtns].hwnd = h;
    g_btns[g_nbtns].old = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)btn_subclass);
    g_btns[g_nbtns].hover = 0;
    g_nbtns++;
}

/* ------------------------------------------------------------------ 日志队列 */

#define LOG_MAX 4000

typedef struct { char level; char *text; } LogLine;
static LogLine g_log[LOG_MAX];
static CRITICAL_SECTION g_mtx;

static char classify(const char *text) {
    if (!strncmp(text, "=====", 5)) return 'h';
    if (!strncmp(text, "[试运行]", strlen("[试运行]"))) return 'p';
    if (strstr(text, "  ->  ") || strstr(text, " -> ")) return 'r';
    return 'i';
}

static void gui_log_sink(int level, const char *text) {
    char stamp[16];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_s(&tmv, &now);
    strftime(stamp, sizeof stamp, "%H:%M:%S", &tmv);

    char *line = fmt("[%s] %s", stamp, text);
    EnterCriticalSection(&g_mtx);
    long idx = g_log_total % LOG_MAX;
    free(g_log[idx].text);
    g_log[idx].level = level ? 'w' : classify(text);
    g_log[idx].text = line;
    g_log_total++;
    LeaveCriticalSection(&g_mtx);

    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_LOG, 0, 0);
}

static int g_log_cursor = 0;

static COLORREF color_for(char level) {
    switch (level) {
    case 'w': return RGB(160, 90, 0);
    case 'r': return RGB(23, 114, 69);
    case 'p': return RGB(107, 63, 160);
    case 'h': return RGB(11, 92, 189);
    default:  return C_TEXT;
    }
}

static void append_rich(const wchar_t *text, COLORREF color) {
    /* 只读的 EDIT / RICHEDIT 会忽略 EM_REPLACESEL，先临时解锁 */
    SendMessageW(h_log, EM_SETREADONLY, FALSE, 0);
    int len = GetWindowTextLengthW(h_log);
    SendMessageW(h_log, EM_SETSEL, (WPARAM)len, (LPARAM)len);

    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof cf);
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_COLOR;
    cf.crTextColor = color;
    SendMessageW(h_log, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

    SendMessageW(h_log, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageW(h_log, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    SendMessageW(h_log, EM_SETREADONLY, TRUE, 0);
}

static void drain_log(void) {
    EnterCriticalSection(&g_mtx);
    long total = g_log_total;
    long start = total > LOG_MAX ? total - LOG_MAX : 0;
    if (g_log_cursor < start) g_log_cursor = (int)start;
    for (long i = g_log_cursor; i < total; i++) {
        LogLine *l = &g_log[i % LOG_MAX];
        if (!l->text) continue;
        wchar_t *w = u8_to_w(l->text);
        append_rich(w, color_for(l->level));
        free(w);
    }
    g_log_cursor = (int)total;
    LeaveCriticalSection(&g_mtx);

    SendMessageW(h_log, EM_SCROLLCARET, 0, 0);
}

static void set_status(const char *fmt2, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt2);
    vsnprintf(buf, sizeof buf, fmt2, ap);
    va_end(ap);
    wchar_t *w = u8_to_w(buf);
    SetWindowTextW(h_status, w);
    free(w);
}

/* ------------------------------------------------------------------ 参数读取 */

typedef struct {
    char path[2048];
    char sep[16];
    int replace;
    int digits;
    int interval_ms;
    int recursive;
    int dry;
    int reprocess;
} Params;

static void get_text_utf8(HWND h, char *out, int cap) {
    int n = GetWindowTextLengthW(h);
    wchar_t *w = xmalloc(sizeof(wchar_t) * (size_t)(n + 1));
    GetWindowTextW(h, w, n + 1);
    char *u = w_to_u8(w);
    snprintf(out, (size_t)cap, "%s", u);
    free(u);
    free(w);
}

static int params_from_ui(Params *p) {
    memset(p, 0, sizeof *p);
    get_text_utf8(h_path, p->path, (int)sizeof p->path);
    get_text_utf8(h_sep, p->sep, (int)sizeof p->sep);

    char num[32];
    get_text_utf8(h_digits, num, (int)sizeof num);
    p->digits = atoi(num);
    if (p->digits < 1 || p->digits > 18) p->digits = DEFAULT_DIGITS;

    get_text_utf8(h_interval, num, (int)sizeof num);
    double sec = atof(num);
    if (sec < 0.2) sec = 0.2;
    p->interval_ms = (int)(sec * 1000);

    p->replace = (int)SendMessageW(h_rb_replace, BM_GETCHECK, 0, 0) == BST_CHECKED;
    p->recursive = (int)SendMessageW(h_recursive, BM_GETCHECK, 0, 0) == BST_CHECKED;
    p->dry = (int)SendMessageW(h_dry, BM_GETCHECK, 0, 0) == BST_CHECKED;
    p->reprocess = (int)SendMessageW(h_reprocess, BM_GETCHECK, 0, 0) == BST_CHECKED;
    return p->path[0] != 0;
}

static void set_busy(int busy) {
    g_busy = busy;
    HWND lockable[] = { h_path, h_browse, h_rb_prefix, h_rb_replace,
                        h_sep, h_digits, h_interval, h_recursive, h_dry,
                        h_reprocess, h_run, h_watch, h_restore };
    for (size_t i = 0; i < sizeof lockable / sizeof lockable[0]; i++)
        if (lockable[i]) {
            EnableWindow(lockable[i], !busy);
            InvalidateRect(lockable[i], NULL, TRUE);
        }
    EnableWindow(h_stop, busy && g_watching);
    InvalidateRect(h_stop, NULL, TRUE);
}

/* ------------------------------------------------------------------ 工作线程 */

static DWORD WINAPI worker_run(LPVOID arg) {
    Params *p = arg;
    char *sp = path_join(p->path, STATE_NAME);
    State st;
    state_init(&st, sp);
    free(sp);
    if (state_load(&st) != 0) logwarn("状态文件内容异常，当作空状态继续");

    Ctx c;
    ctx_init(&c, &st, p->sep, p->digits, p->replace, p->recursive, p->dry, p->reprocess);
    scan_dir(&c, p->path);
    if (!p->dry && st.dirty) state_save(&st);
    g_processed = c.count;
    loginfo(p->dry ? "试运行完成，共 %d 个文件。" : "完成，共处理 %d 个文件。", c.count);
    ctx_free(&c);
    state_free(&st);

    free(p);
    PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
    return 0;
}

static DWORD WINAPI worker_restore(LPVOID arg) {
    Params *p = arg;
    char *sp = path_join(p->path, STATE_NAME);
    State st;
    state_init(&st, sp);
    free(sp);
    if (state_load(&st) != 0) logwarn("状态文件内容异常，当作空状态继续");

    Ctx c;
    ctx_init(&c, &st, p->sep, p->digits, p->replace, p->recursive, p->dry, p->reprocess);
    SkipItem *skips = NULL;
    size_t nskips = 0;
    int n = restore_dir(&c, p->path, 0, &skips, &nskips);
    if (nskips) {
        logwarn("有 %zu 个文件没能恢复：", nskips);
        for (size_t i = 0; i < nskips && i < 5; i++)
            logwarn("    %s —— %s", skips[i].name, skips[i].why);
        for (size_t i = 0; i < nskips; i++) { free(skips[i].name); free(skips[i].why); }
        free(skips);
    }
    if (!p->dry && st.dirty) state_save(&st);
    g_processed = n;
    loginfo(p->dry ? "试运行完成，共 %d 个文件。" : "恢复完成，共 %d 个文件。", n);
    ctx_free(&c);
    state_free(&st);

    free(p);
    PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
    return 0;
}

static DWORD WINAPI worker_watch(LPVOID arg) {
    Params *p = arg;
    char *sp = path_join(p->path, STATE_NAME);
    State st;
    state_init(&st, sp);
    free(sp);
    if (state_load(&st) != 0) logwarn("状态文件内容异常，当作空状态继续");

    Ctx c;
    ctx_init(&c, &st, p->sep, p->digits, p->replace, p->recursive, p->dry, p->reprocess);
    g_processed = 0;
    loginfo("开始监测，点「停止」结束");

    while (!g_stop_flag) {
        int before = c.count;
        scan_dir(&c, p->path);
        if (!p->dry && st.dirty) state_save(&st);
        g_processed += c.count - before;
        if (c.count - before > 0) PostMessageW(g_hwnd, WM_APP_LOG, 0, 0);

        int slept = 0;
        while (!g_stop_flag && slept < p->interval_ms) { sleep_ms(100); slept += 100; }
    }
    loginfo("已停止，本次共处理 %d 个文件。", c.count);
    g_watching = 0;
    ctx_free(&c);
    state_free(&st);
    free(p);
    PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
    return 0;
}

/* ------------------------------------------------------------------ 动作 */

static void start_run(int restore_mode) {
    Params *p = xmalloc(sizeof *p);
    if (!params_from_ui(p)) { free(p); MessageBoxW(g_hwnd, L"请先选择文件夹。", L"提示", MB_ICONINFORMATION); return; }
    if (!fs_is_dir(p->path)) { free(p); MessageBoxW(g_hwnd, L"这个文件夹不存在。", L"提示", MB_ICONWARNING); return; }
    if (g_busy) { free(p); return; }

    if (!p->dry) {
        char *probe = check_writable(p->path);
        if (probe) {
            wchar_t *w = u8_to_w(probe);
            MessageBoxW(g_hwnd, w, L"无法写入这个文件夹", MB_ICONERROR);
            free(w);
            free(probe);
            free(p);
            return;
        }
    }
    if (restore_mode) {
        if (MessageBoxW(g_hwnd, L"把本程序改过名的 mp3 还原成原始文件名？\n\n"
                                L"· 有记录的精确还原；文件被手动改名过也能靠内容指纹认出来\n"
                                L"· 老文件只能摘掉 6 位以上的数字前缀\n"
                                L"· 原名已被占用时自动加 (2)(3) 后缀",
                        L"恢复原名", MB_YESNO | MB_ICONQUESTION) != IDYES) {
            free(p);
            return;
        }
    }

    g_mode = restore_mode ? 2 : 0;
    g_stop_flag = 0;
    set_busy(1);
    HANDLE h = CreateThread(NULL, 0, restore_mode ? worker_restore : worker_run, p, 0, NULL);
    if (!h) { set_busy(0); free(p); MessageBoxW(g_hwnd, L"无法创建线程。", L"错误", MB_ICONERROR); return; }
    CloseHandle(h);
    set_status(restore_mode ? "正在恢复原名……" : "正在执行……");
}

static void start_watch(void) {
    if (g_busy) return;
    Params *p = xmalloc(sizeof *p);
    if (!params_from_ui(p)) { free(p); MessageBoxW(g_hwnd, L"请先选择文件夹。", L"提示", MB_ICONINFORMATION); return; }
    if (!fs_is_dir(p->path)) { free(p); MessageBoxW(g_hwnd, L"这个文件夹不存在。", L"提示", MB_ICONWARNING); return; }

    if (!p->dry) {
        char *probe = check_writable(p->path);
        if (probe) {
            wchar_t *w = u8_to_w(probe);
            MessageBoxW(g_hwnd, w, L"无法写入这个文件夹", MB_ICONERROR);
            free(w); free(probe); free(p);
            return;
        }
    }

    g_mode = 1;
    g_stop_flag = 0;
    g_watching = 1;
    set_busy(1);
    EnableWindow(h_stop, TRUE);
    InvalidateRect(h_stop, NULL, TRUE);
    HANDLE h = CreateThread(NULL, 0, worker_watch, p, 0, NULL);
    if (!h) { g_watching = 0; set_busy(0); free(p); return; }
    CloseHandle(h);
    set_status("监测中……");
}

/* ------------------------------------------------------------------ 目录选择 */

static int pick_folder(wchar_t *out, int cap) {
    BROWSEINFOW bi;
    memset(&bi, 0, sizeof bi);
    bi.hwndOwner = g_hwnd;
    bi.lpszTitle = L"选择要处理的文件夹";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return 0;
    BOOL ok = SHGetPathFromIDListW(pidl, out);
    CoTaskMemFree(pidl);
    return ok && out[0] != 0;
}

/* ------------------------------------------------------------------ 扁平按钮绘制 */

enum { BTN_PRIMARY = 0, BTN_OUTLINE, BTN_GHOST };

static int btn_kind(int id) {
    switch (id) {
    case ID_RUN:     return BTN_PRIMARY;
    case ID_WATCH:   return BTN_OUTLINE;
    case ID_BROWSE:  return BTN_OUTLINE;
    default:         return BTN_GHOST;
    }
}

static void draw_button(const DRAWITEMSTRUCT *dis) {
    int kind = btn_kind((int)dis->CtlID);
    int pressed = (dis->itemState & ODS_SELECTED) != 0;
    int disabled = (dis->itemState & ODS_DISABLED) != 0;
    BtnEntry *e = btn_find(dis->hwndItem);
    int hover = e && e->hover && !disabled;

    COLORREF fill, border, txt;
    if (kind == BTN_PRIMARY) {
        fill = disabled ? RGB(186, 200, 230) : pressed ? C_BLUE_D : hover ? C_BLUE_H : C_BLUE;
        border = fill;
        txt = C_WHITE;
    } else if (kind == BTN_OUTLINE) {
        fill = pressed ? RGB(214, 228, 250) : hover ? RGB(232, 240, 252) : C_WHITE;
        border = disabled ? C_LINE : C_BLUE;
        txt = disabled ? C_DIS : C_BLUE;
        if (disabled) fill = C_BG;
    } else {
        fill = pressed ? C_PRESS : hover ? C_HOVER : C_WHITE;
        border = disabled ? C_LINE : C_LINE;
        txt = disabled ? C_DIS : C_TEXT;
        if (disabled) fill = C_BG;
    }

    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    FillRect(dc, &rc, g_br_bg);

    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ obr = SelectObject(dc, br);
    HGDIOBJ open = SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, S(10), S(10));
    SelectObject(dc, obr);
    SelectObject(dc, open);
    DeleteObject(br);
    DeleteObject(pen);

    wchar_t label[128];
    GetWindowTextW(dis->hwndItem, label, 128);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, txt);
    HFONT ofont = (HFONT)SelectObject(dc, g_font_ui);
    RECT tr = rc;
    if (pressed) OffsetRect(&tr, 0, S(1));
    DrawTextW(dc, label, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, ofont);
}

/* ------------------------------------------------------------------ 布局 */

static void place_edit(HWND h, int x, int y, int w, int hgt) {
    /* 视觉框比控件大 1px，边框由父窗口绘制 */
    if (g_nframes < (int)(sizeof g_frames / sizeof g_frames[0])) {
        RECT *r = &g_frames[g_nframes++];
        r->left = x; r->top = y; r->right = x + w; r->bottom = y + hgt;
    }
    SetWindowPos(h, NULL, x, y, w, hgt, SWP_NOZORDER);
}

static void layout(int cx, int cy) {
    int pad = S(16);
    int row = S(24);
    int y = pad;
    g_nframes = 0;

    /* 第一行：文件夹 */
    SetWindowPos(h_lbl_path, NULL, pad, y + S(4), S(52), S(20), SWP_NOZORDER);
    int pw = cx - pad * 2 - S(52) - S(8) - S(84) - S(8);
    place_edit(h_path, pad + S(52) + S(8), y, pw, row + S(4));
    SetWindowPos(h_browse, NULL, cx - pad - S(84), y, S(84), row + S(4), SWP_NOZORDER);
    y += row + S(4) + S(18);

    /* 选项区（无分组框，用一个小标题分隔） */
    SetWindowPos(h_lbl_opt, NULL, pad, y, S(60), S(18), SWP_NOZORDER);
    y += S(24);

    SetWindowPos(h_rb_prefix, NULL, pad, y, S(180), S(22), SWP_NOZORDER);
    SetWindowPos(h_rb_replace, NULL, pad + S(186), y, S(140), S(22), SWP_NOZORDER);

    int sx = cx - pad - S(300);
    SetWindowPos(h_lbl_sep, NULL, sx, y + S(3), S(44), S(18), SWP_NOZORDER);
    place_edit(h_sep, sx + S(50), y, S(40), row);
    SetWindowPos(h_lbl_example, NULL, sx + S(98), y + S(3), S(200), S(18), SWP_NOZORDER);
    y += S(32);

    SetWindowPos(h_lbl_digits, NULL, pad, y + S(3), S(56), S(18), SWP_NOZORDER);
    place_edit(h_digits, pad + S(60), y, S(48), row);
    SetWindowPos(h_lbl_interval, NULL, pad + S(126), y + S(3), S(56), S(18), SWP_NOZORDER);
    place_edit(h_interval, pad + S(186), y, S(52), row);
    SetWindowPos(h_lbl_sec, NULL, pad + S(244), y + S(3), S(18), S(18), SWP_NOZORDER);
    SetWindowPos(h_recursive, NULL, pad + S(280), y, S(104), S(22), SWP_NOZORDER);
    SetWindowPos(h_dry, NULL, pad + S(392), y, S(140), S(22), SWP_NOZORDER);
    y += S(32);

    SetWindowPos(h_reprocess, NULL, pad, y, S(260), S(22), SWP_NOZORDER);
    y += S(32) + S(8);

    /* 按钮行 */
    int bx = pad;
    SetWindowPos(h_run, NULL, bx, y, S(100), S(32), SWP_NOZORDER); bx += S(108);
    SetWindowPos(h_watch, NULL, bx, y, S(96), S(32), SWP_NOZORDER); bx += S(104);
    SetWindowPos(h_stop, NULL, bx, y, S(72), S(32), SWP_NOZORDER); bx += S(88);
    SetWindowPos(h_restore, NULL, bx, y, S(96), S(32), SWP_NOZORDER);
    SetWindowPos(h_clear, NULL, cx - pad - S(88), y, S(88), S(32), SWP_NOZORDER);
    y += S(32) + S(14);

    /* 日志 + 状态栏 */
    int status_h = S(22);
    int lh = cy - y - status_h - S(12);
    if (lh < S(60)) lh = S(60);
    g_log_rect.left = pad;
    g_log_rect.top = y;
    g_log_rect.right = cx - pad;
    g_log_rect.bottom = y + lh;
    SetWindowPos(h_log, NULL, pad, y, cx - pad * 2, lh, SWP_NOZORDER);
    SetWindowPos(h_status, NULL, pad, cy - status_h - S(2), cx - pad * 2, status_h, SWP_NOZORDER);
}

/* ------------------------------------------------------------------ 创建控件 */

static HWND mk(const wchar_t *cls, const wchar_t *text, DWORD style, DWORD ex,
               int id, HFONT font, int borderless) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style,
                             0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(NULL), NULL);
    if (h && font) SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
    (void)borderless;
    return h;
}

static void create_controls(void) {
    g_font_ui = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
    g_font_log = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             FIXED_PITCH | FF_MODERN, L"Consolas");

    h_lbl_path = mk(L"STATIC", L"文件夹", SS_LEFT, 0, -1, g_font_ui, 0);
    h_path = mk(L"EDIT", L"", ES_AUTOHSCROLL, 0, ID_PATH, g_font_ui, 1);
    h_browse = mk(L"BUTTON", L"浏览…", BS_OWNERDRAW, 0, ID_BROWSE, g_font_ui, 0);

    h_lbl_opt = mk(L"STATIC", L"选项", SS_LEFT, 0, -1, g_font_ui, 0);

    h_rb_prefix = mk(L"BUTTON", L"加在原名前（保留原名）", BS_AUTORADIOBUTTON | WS_GROUP, 0, ID_MODE_PREFIX, g_font_ui, 0);
    h_rb_replace = mk(L"BUTTON", L"只保留数字", BS_AUTORADIOBUTTON, 0, ID_MODE_REPLACE, g_font_ui, 0);
    SendMessageW(h_rb_prefix, BM_SETCHECK, BST_CHECKED, 0);

    h_lbl_sep = mk(L"STATIC", L"分隔符", SS_RIGHT, 0, -1, g_font_ui, 0);
    h_sep = mk(L"EDIT", L"_", ES_CENTER | ES_AUTOHSCROLL, 0, ID_SEP, g_font_ui, 1);
    h_lbl_example = mk(L"STATIC", L"例：123456_歌名.mp3", SS_LEFT, 0, -1, g_font_ui, 0);

    h_lbl_digits = mk(L"STATIC", L"随机位数", SS_LEFT, 0, -1, g_font_ui, 0);
    h_digits = mk(L"EDIT", L"9", ES_CENTER | ES_NUMBER, 0, ID_DIGITS, g_font_ui, 1);
    h_lbl_interval = mk(L"STATIC", L"扫描间隔", SS_LEFT, 0, -1, g_font_ui, 0);
    h_interval = mk(L"EDIT", L"1.0", ES_CENTER, 0, ID_INTERVAL, g_font_ui, 1);
    h_lbl_sec = mk(L"STATIC", L"秒", SS_LEFT, 0, -1, g_font_ui, 0);

    h_recursive = mk(L"BUTTON", L"含子文件夹", BS_AUTOCHECKBOX, 0, ID_RECURSIVE, g_font_ui, 0);
    h_dry = mk(L"BUTTON", L"试运行（不改名）", BS_AUTOCHECKBOX, 0, ID_DRY, g_font_ui, 0);
    h_reprocess = mk(L"BUTTON", L"重新处理已改名的文件（换新编号）", BS_AUTOCHECKBOX, 0, ID_REPROCESS, g_font_ui, 0);

    h_run = mk(L"BUTTON", L"只执行一次", BS_OWNERDRAW, 0, ID_RUN, g_font_ui, 0);
    h_watch = mk(L"BUTTON", L"开始监测", BS_OWNERDRAW, 0, ID_WATCH, g_font_ui, 0);
    h_stop = mk(L"BUTTON", L"停止", BS_OWNERDRAW, 0, ID_STOP, g_font_ui, 0);
    h_restore = mk(L"BUTTON", L"恢复原名", BS_OWNERDRAW, 0, ID_RESTORE, g_font_ui, 0);
    h_clear = mk(L"BUTTON", L"清空日志", BS_OWNERDRAW, 0, ID_CLEAR, g_font_ui, 0);
    EnableWindow(h_stop, FALSE);

    track_button(h_run);
    track_button(h_watch);
    track_button(h_stop);
    track_button(h_restore);
    track_button(h_clear);
    track_button(h_browse);

    h_log = mk(MSFTEDIT_CLASS, L"",
               WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
               0, ID_LOG, g_font_log, 1);
    if (!h_log) {
        h_log = mk(L"EDIT", L"", WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                   0, ID_LOG, g_font_log, 1);
    } else {
        SendMessageW(h_log, EM_SETBKGNDCOLOR, 0, (LPARAM)C_WHITE);
    }

    {
        int ids[] = { ID_PATH, ID_SEP, ID_DIGITS, ID_INTERVAL };
        for (int i = 0; i < 4; i++) {
            HWND e = GetDlgItem(g_hwnd, ids[i]);
            if (e) SendMessageW(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                                MAKELPARAM(S(6), S(6)));
        }
        SendMessageW(h_log, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(S(8), S(8)));
    }

    h_status = mk(L"STATIC", L"就绪：选个文件夹，先试试「只执行一次」", SS_LEFT, 0, ID_STATUS, g_font_ui, 0);
}

/* ------------------------------------------------------------------ 窗口过程 */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        create_controls();
        RECT rc;
        GetClientRect(hwnd, &rc);
        layout(rc.right, rc.bottom);
        return 0;
    }
    case WM_SIZE:
        layout((int)LOWORD(lp), (int)HIWORD(lp));
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;

    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, g_br_bg);
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, g_br_bg);

        /* 输入框和日志区域的 1px 细边框 */
        HPEN pen = CreatePen(PS_SOLID, 1, C_LINE);
        HGDIOBJ open = SelectObject(dc, pen);
        HGDIOBJ obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
        for (int i = 0; i < g_nframes; i++) {
            RECT r = g_frames[i];
            InflateRect(&r, 1, 1);
            Rectangle(dc, r.left, r.top, r.right, r.bottom);
        }
        if (g_log_rect.right > g_log_rect.left) {
            RECT r = g_log_rect;
            InflateRect(&r, 1, 1);
            Rectangle(dc, r.left, r.top, r.right, r.bottom);
        }
        SelectObject(dc, obr);
        SelectObject(dc, open);
        DeleteObject(pen);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, (HWND)lp == h_status ? C_MUTED : C_TEXT);
        return (LRESULT)g_br_bg;
    }
    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)g_br_bg;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, C_WHITE);
        SetTextColor(dc, C_TEXT);
        return (LRESULT)g_br_white;
    }

    case WM_DRAWITEM:
        draw_button((const DRAWITEMSTRUCT *)lp);
        return TRUE;

    case WM_COMMAND: {
        int id = LOWORD(wp);
        switch (id) {
        case ID_BROWSE: {
            wchar_t buf[2048];
            if (pick_folder(buf, 2048)) {
                SetWindowTextW(h_path, buf);
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;
        }
        case ID_RUN:     start_run(0); return 0;
        case ID_WATCH:   start_watch(); return 0;
        case ID_RESTORE: start_run(1); return 0;
        case ID_STOP:
            g_stop_flag = 1;
            EnableWindow(h_stop, FALSE);
            InvalidateRect(h_stop, NULL, TRUE);
            set_status("正在停止……");
            return 0;
        case ID_CLEAR:
            SetWindowTextW(h_log, L"");
            EnterCriticalSection(&g_mtx);
            for (int i = 0; i < LOG_MAX; i++) { free(g_log[i].text); g_log[i].text = NULL; }
            g_log_total = 0;
            g_log_cursor = 0;
            LeaveCriticalSection(&g_mtx);
            return 0;
        case ID_SEP: {
            char sep[16];
            get_text_utf8(h_sep, sep, (int)sizeof sep);
            char buf[128];
            snprintf(buf, sizeof buf, "例：123456%s歌名.mp3", sep);
            wchar_t *w = u8_to_w(buf);
            SetWindowTextW(h_lbl_example, w);
            free(w);
            return 0;
        }
        case ID_MODE_PREFIX:
        case ID_MODE_REPLACE:
            SendMessageW(hwnd, WM_COMMAND, ID_SEP, 0);
            return 0;
        default:
            return 0;
        }
    }

    case WM_APP_LOG:
        drain_log();
        if (g_watching) set_status("监测中…… 已处理 %d 个文件", g_processed);
        return 0;

    case WM_APP_DONE:
        g_busy = 0;
        g_watching = 0;
        set_busy(0);
        if (g_mode == 1)      set_status("已停止，本次共处理 %d 个文件。", g_processed);
        else if (g_mode == 2) set_status("恢复完成，共 %d 个文件。", g_processed);
        else                  set_status("完成，共处理 %d 个文件。", g_processed);
        return 0;

    case WM_CLOSE:
        if (g_busy) {
            if (MessageBoxW(hwnd, L"任务还在跑，确定退出吗？", L"退出",
                            MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
            g_stop_flag = 1;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ 视觉样式 */

/* 没有资源文件，就把 manifest 写到临时目录再激活 ComCtl32 v6 */
static void enable_visual_styles(void) {
    static const char *manifest =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\n"
        "  <assemblyIdentity type=\"win32\" name=\"mp3rn.rename\" version=\"1.0.0.0\""
        " processorArchitecture=\"*\"/>\n"
        "  <dependency><dependentAssembly>\n"
        "    <assemblyIdentity type=\"win32\" name=\"Microsoft.Windows.Common-Controls\""
        " version=\"6.0.0.0\" processorArchitecture=\"*\""
        " publicKeyToken=\"6595b64144ccf1df\" language=\"*\"/>\n"
        "  </dependentAssembly></dependency>\n"
        "</assembly>\n";

    wchar_t tmpdir[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmpdir);
    if (!n) return;
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%smp3rn_win32_manifest.xml", tmpdir);
    path[MAX_PATH - 1] = 0;

    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(f, manifest, (DWORD)strlen(manifest), &wrote, NULL);
    CloseHandle(f);

    ACTCTXW ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cbSize = sizeof ctx;
    ctx.lpSource = path;
    HANDLE act = CreateActCtxW(&ctx);
    if (act != INVALID_HANDLE_VALUE) {
        ULONG_PTR cookie = 0;
        ActivateActCtx(act, &cookie);
    }
}

/* ------------------------------------------------------------------ 入口 */

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    build_argv_utf8();
    argc = win_argc;
    argv = win_argv;

    enable_visual_styles();
    SetProcessDPIAware();

    HDC screen = GetDC(NULL);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);
    if (g_dpi <= 0) g_dpi = 96;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    LoadLibraryW(L"Msftedit.dll");

    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    InitializeCriticalSection(&g_mtx);
    rng_seed();
    g_log_sink = gui_log_sink;

    g_br_bg = CreateSolidBrush(C_BG);
    g_br_white = CreateSolidBrush(C_WHITE);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"MP3RenameWindow";
    /* 图标来自嵌进 exe 的资源（_build/app_res.obj），组图标 ID 为 1 */
    HICON hi_big = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                     S(32), S(32), LR_SHARED);
    HICON hi_small = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                       S(16), S(16), LR_SHARED);
    wc.hIcon = hi_big ? hi_big : LoadIconW(NULL, IDI_APPLICATION);
    wc.hIconSm = hi_small ? hi_small : NULL;
    if (!RegisterClassExW(&wc)) { MessageBoxW(NULL, L"注册窗口类失败", L"错误", MB_ICONERROR); return 1; }

    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"MP3RenameWindow",
                                L"MP3 随机数字改名器",
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, S(940), S(700),
                                NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { MessageBoxW(NULL, L"创建窗口失败", L"错误", MB_ICONERROR); return 1; }

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            MessageBoxW(NULL, L"用法：mp3rn-winningui.exe [文件夹路径]", L"帮助", MB_ICONINFORMATION);
            return 0;
        }
        if (argv[i][0] != '-') {
            wchar_t *w = u8_to_w(argv[i]);
            SetWindowTextW(h_path, w);
            free(w);
            break;
        }
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    set_status("就绪：选个文件夹，先试试「只执行一次」");

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    CoUninitialize();
    return 0;
}
