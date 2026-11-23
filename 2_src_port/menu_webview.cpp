/* menu_webview.cpp - WebView2 host for the embedded HTML settings menu.
 *
 * A dedicated window thread hosts the page compiled into app.rc. BR_Config
 * provides settings shared with the overlay. Messages are strings:
 *   HTML -> host: set:<key>:<int>, col:<index>:r,g,b, act:save|load,
 *                 copy:<text>, drag:<x>,<y>,<dx>,<dy>, dragend:, close:, hello:
 *   host -> HTML: state:<json>, telem:<json>, toast:<text>
 * See msg_cb() for commands and build_state_json() for the returned fields.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdlib>

#include "menu_bridge.h"
#include "app_resources.h"
#include "wv2_cb.h"
#include "rr_game.h"
#include "rr_mem.h"
#include "rr_unity.h"

/* Include the COM declarations before the WebView2 SDK header. */
#include <objbase.h>
#include <rpc.h>
#include <rpcndr.h>
#include <combaseapi.h>
#include "WebView2.h"
#include "wv2_cb_impl.h"
#pragma comment(lib, "WebView2Loader.dll.lib")

/* Menu window state. */
static HWND              g_hwnd   = NULL;
static ICoreWebView2Environment*         g_env  = nullptr;
static ICoreWebView2*                   g_wv   = nullptr;
static ICoreWebView2Controller*          g_ctl  = nullptr;

static HANDLE  g_thread     = NULL;
static DWORD   g_thread_id  = 0;
static volatile LONG g_quit = 0;
static volatile LONG g_visible = 1;
static bool    g_dragging   = false;
static bool    g_page_ready = false;
static HRESULT g_start_error = S_OK;
static HWND    g_prev_fg = NULL;
static POINT   g_drag_origin= {0,0};   /* Window position at the start of a drag. */

static const int WIN_W = 976;
static const int WIN_H = 640;

static void menu_log(const char* stage, HRESULT hr) {
    if (FILE* f = fopen("wv2_error.log", "a")) {
        fprintf(f, "%lu Port menu: %s (0x%08lX)\n",
                GetTickCount(), stage, static_cast<unsigned long>(hr));
        fclose(f);
    }
}

static void menu_failed(const char* stage, HRESULT hr) {
    g_start_error = FAILED(hr) ? hr : E_FAIL;
    menu_log(stage, g_start_error);
    InvalidateRect(g_hwnd, NULL, TRUE);
}

static HRESULT navigate_menu(ICoreWebView2* webview) {
    HMODULE module = GetModuleHandleW(NULL);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_MENU_HTML), MAKEINTRESOURCEW(10));
    if (!resource) return E_FAIL;
    HGLOBAL data = LoadResource(module, resource);
    const char* bytes = static_cast<const char*>(LockResource(data));
    int size = (int)SizeofResource(module, resource);
    if (!bytes || size <= 0) return E_FAIL;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, size, NULL, 0);
    if (!count) return E_FAIL;
    std::wstring html((size_t)count, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, size, &html[0], count))
        return E_FAIL;
    return webview->NavigateToString(html.c_str());
}

/* JSON serialization helpers. */
static void jcat_int(std::string& s, const char* k, int v, bool first=false){
    char b[96]; snprintf(b,sizeof(b),"%s\"%s\":%d", first?"":",",k,v); s+=b;
}

/* Default palette. Index order matches BR_ColorId; 0..14 are the historical
 * values, the appended slots copy the colours that used to be hard-coded at
 * their draw sites. Used by br_get() and by act:reset_colors. */
static const unsigned char g_default_colors[][3] = {
    { 86,168,255}, {255, 86, 86}, {120,255,140}, { 86,255,246},
    {158,158,158}, {255,255,255}, {255,255,255}, {255,255,255},
    {255,255,255}, {255,255,255}, {255,255,255}, {120,255,140},
    {255,176, 66}, {139, 92,246}, {220,220,220},                 /* 0..14 */
    {255,255,255}, {255,255,255},                                /* health, crouch */
    { 86,168,255}, { 86,168,255}, { 86,168,255},                 /* skel, cham, box */
    {230,224,245}, {255,214,110}, {235,235,235},                 /* radar bg, vehicle, loot */
    {230,230,230}, {230,230,230}, {111,232,145},                 /* loot none, common, uncommon */
    {105,178,255}, {211,155,255}, {255,190, 92},                 /* rare, epic, legendary */
    {255,113,149},                                               /* exotic */
    {255,214,110}, {255,181, 65},                                /* vehicle marker, cham */
    {227,215,252},                                               /* self status */
    {255,190, 92}, {200,200,200}, {111,232,145}, {105,178,255},   /* container, door, dropped, vehicle loot */
    {201,155,255},                                               /* radar loot node */
    {180, 92,255}                                                /* hiding objects (World tab) */
};

/* Tripwire: the table above is positional and appends only, so catching a
 * missing row here beats tracking down a zero-filled colour in game. */
typedef char br_default_color_count_check[
    (sizeof(g_default_colors) / sizeof(g_default_colors[0]) == BR_COLOR_COUNT) ? 1 : -1];

/* UI accent colours. The menu page paints itself from these; the renderer
 * never reads them. Used by br_get(), act:reset_colors and the state JSON. */
static const unsigned char g_default_accent[3]    = {180, 92, 255};
static const unsigned char g_default_secondary[3] = {255, 92, 174};

static const char* const g_role_color_keys[BR_ROLE_COLOR_COUNT] = {
    "cop", "robber", "hubber", "civilian", "citizen", "clan", "police"
};

/* Fixed role colours mirrored from rr_draw.c; legend only. */
static const unsigned char g_role_colors[BR_ROLE_COLOR_COUNT][3] = {
    { 90,170,255}, {255, 86, 86}, {220,220,220}, {220,220,220},
    {200,230,200}, {218,153,255}, { 86,255,246}
};

typedef struct {
    const char* key;
    int id;
} COLOR_KEY;

static const COLOR_KEY g_color_keys[] = {
    { "marker",         BR_COL_MARKER },
    { "downed",         BR_COL_DOWNED },
    { "name",           BR_COL_NAME },
    { "weapon",         BR_COL_WEAPON },
    { "dist",           BR_COL_DIST },
    { "distance",       BR_COL_DIST },
    { "armor",          BR_COL_ARMOR },
    { "aim",            BR_COL_AIM },
    { "health",         BR_COL_HEALTH },
    { "crouch",         BR_COL_CROUCH },
    { "skel",           BR_COL_SKEL },
    { "skeleton",       BR_COL_SKEL },
    { "cham",           BR_COL_CHAM },
    { "chams",          BR_COL_CHAM },
    { "box",            BR_COL_BOX },
    { "radar_bg",       BR_COL_RADAR_BG },
    { "radar_vehicle",  BR_COL_RADAR_VEHICLE },
    { "radar_loot",     BR_COL_RADAR_LOOT },
    { "radar_node",     BR_COL_RADAR_NODE },
    { "loot_none",      BR_COL_LOOT_NONE },
    { "loot_common",    BR_COL_LOOT_COMMON },
    { "loot_uncommon",  BR_COL_LOOT_UNCOMMON },
    { "loot_rare",      BR_COL_LOOT_RARE },
    { "loot_epic",      BR_COL_LOOT_EPIC },
    { "loot_legendary", BR_COL_LOOT_LEGENDARY },
    { "loot_exotic",    BR_COL_LOOT_EXOTIC },
    { "vehicle_marker", BR_COL_VEHICLE_MARKER },
    { "vehicle_cham",   BR_COL_VEHICLE_CHAM },
    { "self_status",    BR_COL_SELF_STATUS },
    { "container",      BR_COL_CONTAINER },
    { "door",           BR_COL_DOOR },
    { "dropped_loot",   BR_COL_DROPPED_LOOT },
    { "vehicle_loot",   BR_COL_VEHICLE_LOOT },
    { "hide_object",    BR_COL_HIDE_OBJECT },
    { "hide_objects",   BR_COL_HIDE_OBJECT },
};

/* Numeric keys stay the fast path used by the embedded page; names are a
 * convenience for external tooling. Unknown keys resolve to -1. */
static int color_index_for_key(const char* key) {
    if (!key || !*key) return -1;
    if (*key >= '0' && *key <= '9') {
        char* end = nullptr;
        long index = strtol(key, &end, 10);
        if (end && !*end && index >= 0 && index < BR_COLOR_COUNT) return (int)index;
        return -1;
    }
    for (size_t i = 0; i < sizeof(g_color_keys) / sizeof(g_color_keys[0]); ++i)
        if (!strcmp(key, g_color_keys[i].key)) return g_color_keys[i].id;
    return -1;
}

/* col: writes either a palette slot or one of the two UI accents. Returns the
 * three bytes to overwrite, or null when the key is unknown. */
static unsigned char* color_target(BR_Config* b, const char* key) {
    if (!key || !*key) return NULL;
    if (!strcmp(key, "accent")) return b->accent.rgb;
    if (!strcmp(key, "secondary")) return b->secondary.rgb;
    int index = color_index_for_key(key);
    if (index < 0 || index >= BR_COLOR_COUNT) return NULL;
    return b->color[index].rgb;
}


static std::string build_state_json(BR_Config* b) {
    std::string s("{");
#define BR_JSON(field, initial, minimum, maximum) jcat_int(s, #field, b->field, s.size() == 1);
    BR_SETTINGS(BR_JSON)
#undef BR_JSON
    s += ",\"colors\":{";
    for (int i = 0; i < BR_COLOR_COUNT; ++i) {
        char rgb[40];
        snprintf(rgb, sizeof(rgb), "%s\"%d\":[%u,%u,%u]", i ? "," : "", i,
                 (unsigned)b->color[i].rgb[0], (unsigned)b->color[i].rgb[1], (unsigned)b->color[i].rgb[2]);
        s += rgb;
    }
    s += "},\"accent\":[";
    {
        char rgb[64];
        snprintf(rgb, sizeof(rgb), "%u,%u,%u],\"secondary\":[%u,%u,%u],\"role_colors\":{",
                 (unsigned)b->accent.rgb[0], (unsigned)b->accent.rgb[1], (unsigned)b->accent.rgb[2],
                 (unsigned)b->secondary.rgb[0], (unsigned)b->secondary.rgb[1], (unsigned)b->secondary.rgb[2]);
        s += rgb;
    }
    for (int i = 0; i < BR_ROLE_COLOR_COUNT; ++i) {
        char rgb[64];
        snprintf(rgb, sizeof(rgb), "%s\"%s\":[%u,%u,%u]", i ? "," : "", g_role_color_keys[i],
                 (unsigned)b->role_color[i].rgb[0], (unsigned)b->role_color[i].rgb[1],
                 (unsigned)b->role_color[i].rgb[2]);
        s += rgb;
    }
    s += "}}";
    return s;
}

static std::string build_telem_json(BR_Config* b) {
    std::string s("{");
    jcat_int(s,"pid",b->telem_game_pid,1);
    jcat_int(s,"targets",b->telem_targets);
    jcat_int(s,"los",b->telem_los_state);
    /* Live World-tab counters; extras_refresh() publishes them on the worker. */
    jcat_int(s,"cop_count",b->telem_cop_count);
    jcat_int(s,"slot_total",b->telem_slot_total);
    s += "}";
    return s;
}

static void wv_send(const std::string& msg){
    if(!g_wv) return;
    std::wstring w(msg.begin(), msg.end());
    g_wv->PostWebMessageAsString(w.c_str());
}

/* Integer setting fields are resolved by name through the shared schema. */
/* Messages sent from the HTML page. */
static HRESULT STDMETHODCALLTYPE msg_cb(ICoreWebView2* sender,
    ICoreWebView2WebMessageReceivedEventArgs* args, void* ctx)
{
    (void)sender;(void)ctx;
    if(!args) return S_OK;
    LPWSTR w = nullptr;
    if(FAILED(args->TryGetWebMessageAsString(&w)) || !w) return S_OK;
    std::string msg;
    for(const wchar_t* p=w; *p; ++p){
        if(*p<128) msg+=(char)*p;
    }
    CoTaskMemFree(w);
    BR_Config* b = br_get();

    /* Split the message at the first colon into command and body. */
    size_t c1 = msg.find(':');
    if(c1==std::string::npos) return S_OK;
    std::string cmd = msg.substr(0,c1), body = msg.substr(c1+1);

    if (cmd == "set") {
        size_t c2 = body.find(':');
        if (c2 != std::string::npos)
            br_config_set(b, body.substr(0,c2).c_str(), atoi(body.substr(c2+1).c_str()));
    }
    else if (cmd == "col") {
        /* Primary form is col:<index>:r,g,b. Named keys, plus the two UI
         * accents, are accepted as a fallback for external tooling; unknown
         * keys are ignored. */
        size_t cut = body.find(':');
        if (cut != std::string::npos) {
            unsigned char* target = color_target(b, body.substr(0, cut).c_str());
            int red = -1, green = -1, blue = -1;
            if (target &&
                sscanf(body.c_str() + cut + 1, "%d,%d,%d", &red, &green, &blue) == 3 &&
                (unsigned)red <= 255 && (unsigned)green <= 255 && (unsigned)blue <= 255) {
                target[0] = (unsigned char)red;
                target[1] = (unsigned char)green;
                target[2] = (unsigned char)blue;
            }
        }
    }
    else if (cmd == "act") {
        if (body == "save") {
            wv_send(br_config_save() ? "toast:Settings saved" : "toast:Could not save CoreRR.cfg");
        } else if (body == "load") {
            if (br_config_load()) {
                wv_send("state:" + build_state_json(b));
                wv_send("toast:Settings loaded");
            } else {
                wv_send("toast:Could not load CoreRR.cfg");
            }
        } else if (body == "reset_colors") {
            /* Restore the editable palette only; reserved slots keep their
             * historical values so old profiles stay aligned. */
            for (int i = 0; i < BR_COLOR_COUNT; ++i) {
                if (br_color_is_legacy(i)) continue;
                b->color[i].rgb[0] = g_default_colors[i][0];
                b->color[i].rgb[1] = g_default_colors[i][1];
                b->color[i].rgb[2] = g_default_colors[i][2];
            }
            b->accent.rgb[0] = g_default_accent[0];
            b->accent.rgb[1] = g_default_accent[1];
            b->accent.rgb[2] = g_default_accent[2];
            b->secondary.rgb[0] = g_default_secondary[0];
            b->secondary.rgb[1] = g_default_secondary[1];
            b->secondary.rgb[2] = g_default_secondary[2];
            wv_send("state:" + build_state_json(b));
            wv_send("toast:Colors reset to defaults");
        }
    }
    else if(cmd=="drag"){
        /* The page sends total displacement from the start of the drag. Apply it
         * to the original window position; adding it to the current position would
         * count earlier movement again.
         */
        int sx=0, sy=0, dx=0, dy=0;
        if(sscanf(body.c_str(),"%d,%d,%d,%d",&sx,&sy,&dx,&dy)==4){
            if(!g_dragging){
                g_dragging=true;
                /* Remember the window position once, when the drag begins. */
                RECT wr; GetWindowRect(g_hwnd,&wr);
                g_drag_origin.x=wr.left; g_drag_origin.y=wr.top;
            }
            SetWindowPos(g_hwnd,NULL,
                         g_drag_origin.x+dx, g_drag_origin.y+dy,
                         0,0, SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
            (void)sx; (void)sy;
        }
    }
    else if(cmd=="dragend"){
        g_dragging=false;
        g_drag_origin.x=g_drag_origin.y=0;
    }
    else if(cmd=="close"){
        br_set_menu_visible(0);
    }
    else if (cmd == "hello") {
        if (!g_page_ready) menu_log("embedded menu ready", S_OK);
        g_page_ready = true;
        g_start_error = S_OK;
        wv_send("state:" + build_state_json(b));
        wv_send("telem:" + build_telem_json(b));
    }
    return S_OK;
}

/* The WebView2 event handler above applies page commands to shared settings. */

/* Window messages and dragging. */
static LRESULT WINAPI wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l){
    switch(m){
    case WM_DESTROY: PostQuitMessage(0); break;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(h, &paint);
        if (!g_page_ready) {
            RECT rect;
            GetClientRect(h, &rect);
            FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            SetTextColor(dc, RGB(241, 235, 251));
            SetBkMode(dc, TRANSPARENT);
            const wchar_t* text = FAILED(g_start_error)
                ? L"Vinhasxyz could not open WebView2. See wv2_error.log. Press F1 to close."
                : L"Loading Vinhasxyz...";
            DrawTextW(dc, text, -1, &rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        }
        EndPaint(h, &paint);
        return 0;
    }
    }
    return DefWindowProcW(h,m,w,l);
}

static HRESULT controller_ready(HRESULT hr, ICoreWebView2Controller* controller) {
    if (FAILED(hr) || !controller) {
        menu_failed("create browser controller", hr);
        return S_OK;
    }
    g_ctl = controller;
    g_ctl->AddRef();
    RECT bounds;
    GetClientRect(g_hwnd, &bounds);
    g_ctl->put_Bounds(bounds);
    g_ctl->put_IsVisible(IsWindowVisible(g_hwnd));

    hr = g_ctl->get_CoreWebView2(&g_wv);
    if (SUCCEEDED(hr) && g_wv) {
        auto handler = MakeMsgCb([](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) {
            return msg_cb(sender, args, nullptr);
        });
        EventRegistrationToken token{};
        hr = g_wv->add_WebMessageReceived(handler, &token);
        handler->Release();
        if (SUCCEEDED(hr)) hr = navigate_menu(g_wv);
    } else if (SUCCEEDED(hr)) {
        hr = E_FAIL;
    }
    if (FAILED(hr)) {
        g_ctl->put_IsVisible(FALSE);
        menu_failed("open embedded menu", hr);
    }
    return S_OK;
}

static void start_webview(const wchar_t* data_dir, bool allow_retry);

static void browser_start_failed(HRESULT hr, bool allow_retry) {
    wchar_t fallback[MAX_PATH];
    DWORD length = GetTempPathW(_countof(fallback), fallback);
    if (allow_retry && length && length < _countof(fallback) &&
        swprintf_s(fallback + length, _countof(fallback) - length,
                   L"RRToolPortWebView2_%lu", GetCurrentProcessId()) >= 0) {
        menu_log("retry browser with a separate profile", hr);
        start_webview(fallback, false);
        return;
    }
    menu_failed("create browser environment", hr);
}

static void start_webview(const wchar_t* data_dir, bool allow_retry) {
    auto handler = MakeEnvCb([allow_retry](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
        if (FAILED(hr) || !env) {
            browser_start_failed(FAILED(hr) ? hr : E_FAIL, allow_retry);
            return S_OK;
        }
        g_env = env;
        g_env->AddRef();
        auto controller = MakeCtlCb(controller_ready);
        hr = env->CreateCoreWebView2Controller(g_hwnd, controller);
        controller->Release();
        if (FAILED(hr)) menu_failed("request browser controller", hr);
        return S_OK;
    });
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, data_dir, nullptr, handler);
    handler->Release();
    if (FAILED(hr)) browser_start_failed(hr, allow_retry);
}

/* Menu window thread. */
static DWORD WINAPI menu_thread(LPVOID){
    /* Initialize COM on this thread before creating WebView2. A failed COM
     * initialization can leave the host window visible with no rendered page.
     */
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) { menu_log("initialize COM", com); return 1; }
    WNDCLASSEXW wc={0};
    wc.cbSize=sizeof(wc); wc.style=CS_CLASSDC;
    wc.lpfnWndProc=wnd_proc; wc.hInstance=GetModuleHandleW(NULL);
    wc.lpszClassName=L"mrzvCoreWebMenu";
    wc.hCursor=LoadCursorW(NULL,(LPCWSTR)IDC_ARROW);
    RegisterClassExW(&wc);

    int sw=GetSystemMetrics(SM_CXSCREEN), sh=GetSystemMetrics(SM_CYSCREEN);
    g_hwnd=CreateWindowExW(
        WS_EX_TOPMOST|WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"mrzvCore",
        WS_POPUP,
        sw/2-WIN_W/2, 80, WIN_W, WIN_H,
        NULL,NULL,wc.hInstance,NULL);
    if(!g_hwnd) {
        menu_log("create menu window", HRESULT_FROM_WIN32(GetLastError()));
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        CoUninitialize();
        return 1;
    }

    wchar_t data_dir[MAX_PATH];
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", data_dir, _countof(data_dir));
    if (!length || length >= _countof(data_dir))
        length = GetTempPathW(_countof(data_dir), data_dir);
    if (!length || length >= _countof(data_dir) ||
        wcscat_s(data_dir, L"\\RRToolPortWebView2") != 0) {
        menu_failed("resolve browser profile directory", E_FAIL);
    } else {
        start_webview(data_dir, true);
    }

    BR_Config* b = br_get();
    DWORD last_telem=0;
    DWORD shown_at = 0;

    MSG msg; ZeroMemory(&msg,sizeof(msg));
    while(msg.message!=WM_QUIT && !g_quit){
        if(PeekMessageW(&msg,NULL,0,0,PM_REMOVE)){
            TranslateMessage(&msg); DispatchMessageW(&msg); continue;
        }
        if (!InterlockedCompareExchange(&g_visible, 0, 0)) {
            if (IsWindowVisible(g_hwnd)) {
                if (g_ctl) g_ctl->put_IsVisible(FALSE);
                ShowWindow(g_hwnd, SW_HIDE);
                if (g_prev_fg && IsWindow(g_prev_fg)) SetForegroundWindow(g_prev_fg);
                g_prev_fg = NULL;
            }
            MsgWaitForMultipleObjectsEx(0, NULL, 80, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            continue;
        }
        if (!IsWindowVisible(g_hwnd)) {
            shown_at = GetTickCount();
            g_prev_fg = GetForegroundWindow();
            SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
            if (g_ctl && SUCCEEDED(g_start_error)) g_ctl->put_IsVisible(TRUE);
        }

        DWORD now=GetTickCount();
        if (!g_page_ready && SUCCEEDED(g_start_error) && now - shown_at > 15000) {
            if (g_ctl) g_ctl->put_IsVisible(FALSE);
            menu_failed("wait for embedded menu", HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        }
        if(g_wv && now-last_telem>500){
            last_telem=now;
            wv_send("telem:"+build_telem_json(b));
        }
        /* Wake for input immediately instead of leaving messages queued until
           the fixed sleep ends. The timeout still services telemetry. */
        MsgWaitForMultipleObjectsEx(0, NULL, 15, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    if(g_ctl){ g_ctl->Close(); g_ctl->Release(); g_ctl=nullptr; }
    if(g_wv){ g_wv->Release(); g_wv=nullptr; }
    if(g_env){ g_env->Release(); g_env=nullptr; }
    DestroyWindow(g_hwnd);
    g_hwnd = NULL;
    UnregisterClassW(L"mrzvCoreWebMenu",GetModuleHandleW(NULL));
    CoUninitialize();
    return 0;
}

/* Public menu bridge. */
static BR_Config g_br_inst;
static volatile LONG g_br_inited = 0;

BR_Config* br_get(void){
    if(InterlockedCompareExchange(&g_br_inited,1,0)==0){
        ZeroMemory(&g_br_inst,sizeof(g_br_inst));
        g_br_inst.magic=BR_MAGIC; g_br_inst.version=BR_VERSION;
#define BR_DEFAULT(field, initial, minimum, maximum) g_br_inst.field = initial;
        BR_SETTINGS(BR_DEFAULT)
#undef BR_DEFAULT
        g_br_inst.menu_open = 1;
        for (int i = 0; i < BR_COLOR_COUNT; ++i) {
            g_br_inst.color[i].rgb[0] = g_default_colors[i][0];
            g_br_inst.color[i].rgb[1] = g_default_colors[i][1];
            g_br_inst.color[i].rgb[2] = g_default_colors[i][2];
        }
        for (int i = 0; i < BR_ROLE_COLOR_COUNT; ++i) {
            g_br_inst.role_color[i].rgb[0] = g_role_colors[i][0];
            g_br_inst.role_color[i].rgb[1] = g_role_colors[i][1];
            g_br_inst.role_color[i].rgb[2] = g_role_colors[i][2];
        }
        g_br_inst.accent.rgb[0] = g_default_accent[0];
        g_br_inst.accent.rgb[1] = g_default_accent[1];
        g_br_inst.accent.rgb[2] = g_default_accent[2];
        g_br_inst.secondary.rgb[0] = g_default_secondary[0];
        g_br_inst.secondary.rgb[1] = g_default_secondary[1];
        g_br_inst.secondary.rgb[2] = g_default_secondary[2];
    }
    return &g_br_inst;
}

int br_start_menu(void){
    if(g_thread) return 0;
    g_quit=0;
    g_thread=CreateThread(NULL,0,menu_thread,NULL,0,&g_thread_id);
    return g_thread?0:1;
}
void br_stop_menu(void){
    if(!g_thread) return;
    g_quit=1;
    if(g_hwnd) PostMessageW(g_hwnd,WM_NULL,0,0);
    WaitForSingleObject(g_thread,3000);
    CloseHandle(g_thread); g_thread=NULL;
}
void br_request_toggle(void){
    br_set_menu_visible(!InterlockedCompareExchange(&g_visible, 0, 0));
}

void br_set_menu_visible(int visible) {
    br_get()->menu_open = visible != 0;
    InterlockedExchange(&g_visible, visible != 0);
    if (g_hwnd) PostMessageW(g_hwnd, WM_NULL, 0, 0);
}
