// Installer for the Dead Space (2011 mobile) PC port.
//
// One window: where to install, and which APK to take the game data from (none is shipped: the user
// supplies their own Dead Space APK). It writes deadspace.exe (embedded here as a resource), imports the
// APK's assets and game library into <folder>\gamedata, makes Start menu / desktop shortcuts and registers
// an uninstaller for the current user. No administrator rights are needed: the default folder is under
// %LOCALAPPDATA%\Programs, and the game keeps its saves and settings next to its exe.
//
//   setup.exe                 the installer
//   uninstall.exe             (a copy of it, in the install folder) removes the game again
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "apk.h"
#include "log.h"

namespace fs = std::filesystem;

#define APP_NAME L"Dead Space Mobile PC Port"
#define APP_VERSION L"1.0.0"
#define UNINSTALL_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\DeadSpaceMobilePC"

// ---- logging used by the APK importer: the last error line is kept for the message box
namespace logging {
bool g_verbose = false;
static std::string g_last_error;
void write(char level, const char* fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (level == 'E') g_last_error = b;
}
void open_file(const char*) {}
}  // namespace logging

namespace {

enum { ID_DIR = 100, ID_DIR_BROWSE, ID_APK, ID_APK_BROWSE, ID_DESKTOP, ID_INSTALL, ID_STATUS, ID_KEEP };
const UINT WM_DONE = WM_APP + 1;

HWND g_wnd, g_dir, g_apk, g_desktop, g_install, g_status, g_keep;
HFONT g_font;
std::atomic<bool> g_busy{false};
std::wstring g_result;  // error text of the install thread ("" = success)
std::wstring g_installed_dir;

std::wstring text_of(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(size_t(n), L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    return s;
}
std::string utf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}
std::wstring known_folder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}
std::wstring self_path() {
    wchar_t b[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, b, sizeof b / sizeof *b);
    return b;
}
bool has_game_data(const std::wstring& dir) {
    std::error_code ec;
    return fs::exists(fs::path(dir) / L"gamedata" / L"libDeadSpace.so", ec);
}

bool make_shortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& workdir, const std::wstring& args = L"") {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&sl)))) return false;
    sl->SetPath(target.c_str());
    sl->SetWorkingDirectory(workdir.c_str());
    sl->SetArguments(args.c_str());
    sl->SetDescription(APP_NAME);
    IPersistFile* pf = nullptr;
    bool ok = false;
    if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&pf)))) {
        ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
        pf->Release();
    }
    sl->Release();
    return ok;
}

void set_reg(HKEY k, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), DWORD((v.size() + 1) * sizeof(wchar_t)));
}

void status(const wchar_t* s) { SetWindowTextW(g_status, s); }

// ---- install (worker thread)
void install(std::wstring dir, std::wstring apk, bool desktop) {
    std::error_code ec;
    std::wstring err;
    try {
        fs::create_directories(dir, ec);
        if (ec) throw std::wstring(L"Cannot create the folder:\n") + dir;

        // (the game data first: if the file turns out not to be the right APK, nothing has been installed)
        if (!apk.empty()) {
            status(L"Importing the game data from your APK (this takes a moment)...");
            const fs::path gd = fs::path(dir) / L"gamedata";
            fs::remove_all(gd, ec);  // (a re-install with a different APK starts clean)
            logging::g_last_error.clear();
            if (!apk::import(utf8(apk), utf8(gd.wstring()))) {
                fs::remove_all(gd, ec);
                throw std::wstring(L"The game data could not be imported from that file.\n\n") + wide(logging::g_last_error) +
                    L"\n\nIt has to be the Android APK of Dead Space (2011, EA / IronMonkey).";
            }
        }
        status(L"Copying the game program...");
        HRSRC res = FindResourceW(nullptr, L"PAYLOAD_EXE", RT_RCDATA);
        HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
        const void* data = mem ? LockResource(mem) : nullptr;
        const DWORD size = res ? SizeofResource(nullptr, res) : 0;
        if (!data || !size) throw std::wstring(L"This installer is damaged (no game program inside it).");
        {
            std::ofstream o(fs::path(dir) / L"deadspace.exe", std::ios::binary | std::ios::trunc);
            o.write(static_cast<const char*>(data), std::streamsize(size));
            if (!o) throw std::wstring(L"Cannot write deadspace.exe. Is the game running, or the folder read-only?");
        }

        fs::create_directories(fs::path(dir) / L"mods" / L"textures", ec);

        status(L"Creating shortcuts...");
        const std::wstring exe = (fs::path(dir) / L"deadspace.exe").wstring(), un = (fs::path(dir) / L"uninstall.exe").wstring();
        CopyFileW(self_path().c_str(), un.c_str(), FALSE);
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const std::wstring programs = known_folder(FOLDERID_Programs);
        if (!programs.empty()) make_shortcut(programs + L"\\Dead Space Mobile PC Port.lnk", exe, dir);
        if (desktop) make_shortcut(known_folder(FOLDERID_Desktop) + L"\\Dead Space Mobile PC Port.lnk", exe, dir);
        CoUninitialize();

        HKEY k;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, UNINSTALL_KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
            set_reg(k, L"DisplayName", APP_NAME);
            set_reg(k, L"DisplayVersion", APP_VERSION);
            set_reg(k, L"Publisher", L"Dead Space Mobile PC Port (fan project)");
            set_reg(k, L"InstallLocation", dir);
            set_reg(k, L"DisplayIcon", exe);
            set_reg(k, L"UninstallString", L"\"" + un + L"\" /uninstall");
            DWORD one = 1;
            RegSetValueExW(k, L"NoModify", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof one);
            RegSetValueExW(k, L"NoRepair", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof one);
            RegCloseKey(k);
        }
    } catch (const std::wstring& e) {
        err = e;
    } catch (const std::exception& e) {
        err = L"Unexpected error: " + wide(e.what());
    }
    g_result = err;
    g_installed_dir = dir;
    PostMessageW(g_wnd, WM_DONE, 0, 0);
}

// ---- uninstall
// Runs from a copy in %TEMP% (a program cannot delete its own folder while it runs from it).
int uninstall_run(const std::wstring& dir, bool quiet = false) {
    if (!quiet && MessageBoxW(nullptr, (std::wstring(L"Remove ") + APP_NAME + L" from\n" + dir + L" ?").c_str(), APP_NAME, MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
    const bool saves = !quiet && MessageBoxW(nullptr, L"Also delete your saved games and settings?\n\n(Choose No to keep the \"userdata\" folder.)", APP_NAME,
                                   MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
    std::error_code ec;
    const fs::path d(dir);
    if (!fs::exists(d / L"deadspace.exe", ec) && !fs::exists(d / L"uninstall.exe", ec)) {
        if (!quiet) MessageBoxW(nullptr, L"The game was not found in that folder; nothing was removed.", APP_NAME, MB_ICONWARNING);
        return 1;
    }
    Sleep(600);  // (the uninstall.exe that started this copy is exiting)
    for (const wchar_t* f : {L"deadspace.exe", L"uninstall.exe", L"deadspace.log", L"stderr.log", L"crash.dmp"}) fs::remove(d / f, ec);
    fs::remove_all(d / L"gamedata", ec);
    if (saves) fs::remove_all(d / L"userdata", ec);
    // texture packs are the user's own files: the folder goes only if nothing but the game's note is in it
    {
        const fs::path t = d / L"mods" / L"textures";
        fs::remove(t / L"README.txt", ec);
        fs::remove(t, ec);
        fs::remove(d / L"mods", ec);
    }
    fs::remove(d, ec);  // (only if empty now)
    DeleteFileW((known_folder(FOLDERID_Programs) + L"\\Dead Space Mobile PC Port.lnk").c_str());
    DeleteFileW((known_folder(FOLDERID_Desktop) + L"\\Dead Space Mobile PC Port.lnk").c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, UNINSTALL_KEY);
    if (!quiet) MessageBoxW(nullptr, saves ? L"The game has been removed." : L"The game has been removed. Your saves and settings were kept.", APP_NAME, MB_ICONINFORMATION);
    return 0;
}
int uninstall_start() {
    const std::wstring self = self_path(), dir = fs::path(self).parent_path().wstring();
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring copy = std::wstring(tmp) + L"deadspace-port-uninstall.exe";
    if (!CopyFileW(self.c_str(), copy.c_str(), FALSE)) return uninstall_run(dir);
    const std::wstring args = L"/uninstall-run \"" + dir + L"\"";
    ShellExecuteW(nullptr, L"open", copy.c_str(), args.c_str(), tmp, SW_SHOWNORMAL);
    return 0;
}

// ---- window
HWND make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(wcscmp(cls, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_wnd,
                             reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, WPARAM(g_font), TRUE);
    return c;
}

void refresh_keep() {
    const bool have = has_game_data(text_of(g_dir));
    ShowWindow(g_keep, have ? SW_SHOW : SW_HIDE);
}

int CALLBACK browse_cb(HWND h, UINT msg, LPARAM, LPARAM data) {
    if (msg == BFFM_INITIALIZED) SendMessageW(h, BFFM_SETSELECTIONW, TRUE, data);
    return 0;
}

void on_install() {
    std::wstring dir = text_of(g_dir), apk = text_of(g_apk);
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L' ')) dir.pop_back();
    if (dir.empty()) {
        MessageBoxW(g_wnd, L"Choose a folder to install the game into.", APP_NAME, MB_ICONWARNING);
        return;
    }
    std::error_code ec;
    if (apk.empty() && !has_game_data(dir)) {
        MessageBoxW(g_wnd,
                    L"Choose your Dead Space APK file.\n\nThe game's data is not included with this port: it is taken from your own copy of the "
                    L"Android game (Dead Space, 2011, EA / IronMonkey).",
                    APP_NAME, MB_ICONWARNING);
        return;
    }
    if (!apk.empty() && !fs::is_regular_file(apk, ec)) {
        MessageBoxW(g_wnd, L"That APK file does not exist.", APP_NAME, MB_ICONWARNING);
        return;
    }
    g_busy = true;
    for (HWND h : {g_dir, g_apk, g_install, g_desktop, GetDlgItem(g_wnd, ID_DIR_BROWSE), GetDlgItem(g_wnd, ID_APK_BROWSE)}) EnableWindow(h, FALSE);
    std::thread(install, dir, apk, SendMessageW(g_desktop, BM_GETCHECK, 0, 0) == BST_CHECKED).detach();
}

LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == ID_DIR_BROWSE) {
            const std::wstring cur = text_of(g_dir);
            BROWSEINFOW bi{};
            bi.hwndOwner = h;
            bi.lpszTitle = L"Folder to install the game into";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            bi.lpfn = browse_cb;
            bi.lParam = LPARAM(cur.c_str());
            if (PIDLIST_ABSOLUTE p = SHBrowseForFolderW(&bi)) {
                wchar_t path[MAX_PATH];
                if (SHGetPathFromIDListW(p, path)) SetWindowTextW(g_dir, path);
                CoTaskMemFree(p);
            }
            refresh_keep();
        } else if (LOWORD(wp) == ID_APK_BROWSE) {
            wchar_t file[MAX_PATH * 2] = L"";
            OPENFILENAMEW of{};
            of.lStructSize = sizeof of;
            of.hwndOwner = h;
            of.lpstrFilter = L"Android packages (*.apk)\0*.apk\0All files\0*.*\0";
            of.lpstrFile = file;
            of.nMaxFile = sizeof file / sizeof *file;
            of.lpstrTitle = L"Your Dead Space APK";
            of.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&of)) SetWindowTextW(g_apk, file);
        } else if (LOWORD(wp) == ID_DIR && HIWORD(wp) == EN_CHANGE) {
            refresh_keep();
        } else if (LOWORD(wp) == ID_INSTALL && !g_busy) {
            on_install();
        }
        return 0;
    case WM_DONE: {
        g_busy = false;
        if (g_result.empty()) {
            status(L"Installed.");
            if (MessageBoxW(h, (std::wstring(APP_NAME) + L" is installed.\n\nStart the game now?").c_str(), APP_NAME, MB_YESNO | MB_ICONINFORMATION) == IDYES)
                ShellExecuteW(nullptr, L"open", (g_installed_dir + L"\\deadspace.exe").c_str(), nullptr, g_installed_dir.c_str(), SW_SHOWNORMAL);
            DestroyWindow(h);
        } else {
            status(L"Not installed.");
            MessageBoxW(h, g_result.c_str(), APP_NAME, MB_ICONERROR);
            for (HWND c : {g_dir, g_apk, g_install, g_desktop, GetDlgItem(h, ID_DIR_BROWSE), GetDlgItem(h, ID_APK_BROWSE)}) EnableWindow(c, TRUE);
        }
        return 0;
    }
    case WM_CLOSE:
        if (g_busy) return 0;  // (not in the middle of copying)
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int run_installer(HINSTANCE inst) {
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"DeadSpacePortSetup";
    RegisterClassW(&wc);
    const int W = 620, H = 400;
    g_wnd = CreateWindowExW(0, wc.lpszClassName, APP_NAME L" - Setup", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, (GetSystemMetrics(SM_CXSCREEN) - W) / 2,
                            (GetSystemMetrics(SM_CYSCREEN) - H) / 2, W, H, nullptr, nullptr, inst, nullptr);

    make(L"STATIC",
         L"This installs the PC port of Dead Space (the 2011 mobile game by EA / IronMonkey).\r\n\r\n"
         L"The game itself is not included. You need your own copy of the Android game as an APK file; "
         L"its data is copied from that file into the folder below during installation.",
         0, 20, 16, 570, 74, 0);
    make(L"STATIC", L"Install to:", 0, 20, 100, 570, 18, 0);
    g_dir = make(L"EDIT", (known_folder(FOLDERID_LocalAppData) + L"\\Programs\\Dead Space Mobile PC Port").c_str(), ES_AUTOHSCROLL | WS_TABSTOP, 20, 120, 470, 24, ID_DIR);
    make(L"BUTTON", L"Browse...", WS_TABSTOP, 500, 119, 90, 26, ID_DIR_BROWSE);
    make(L"STATIC", L"Your Dead Space APK file:", 0, 20, 160, 570, 18, 0);
    g_apk = make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 20, 180, 470, 24, ID_APK);
    make(L"BUTTON", L"Browse...", WS_TABSTOP, 500, 179, 90, 26, ID_APK_BROWSE);
    g_keep = make(L"STATIC", L"Game data is already installed in this folder: leave the APK empty to keep it (update the program only).", 0, 20, 210, 570, 34, ID_KEEP);
    g_desktop = make(L"BUTTON", L"Create a desktop shortcut", BS_AUTOCHECKBOX | WS_TABSTOP, 20, 250, 300, 22, ID_DESKTOP);
    SendMessageW(g_desktop, BM_SETCHECK, BST_CHECKED, 0);
    g_status = make(L"STATIC", L"Needs about 400 MB of free space. No administrator rights are required.", 0, 20, 290, 570, 20, ID_STATUS);
    g_install = make(L"BUTTON", L"Install", BS_DEFPUSHBUTTON | WS_TABSTOP, 470, 318, 120, 32, ID_INSTALL);
    refresh_keep();

    ShowWindow(g_wnd, SW_SHOW);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(g_wnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CoUninitialize();
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    // (/quiet: no questions, saves kept; used by the tests)
    if (argc >= 3 && !wcscmp(argv[1], L"/uninstall-run")) return uninstall_run(argv[2], argc >= 4 && !wcscmp(argv[3], L"/quiet"));
    if (argc >= 2 && !wcscmp(argv[1], L"/uninstall")) return uninstall_start();
    // dev / CI: setup.exe /silent <folder> <apk>   (no window; exit code 0 = installed)
    if (argc >= 4 && !wcscmp(argv[1], L"/silent")) {
        g_wnd = nullptr;
        g_status = nullptr;
        std::thread t(install, std::wstring(argv[2]), std::wstring(argv[3]), false);
        t.join();
        return g_result.empty() ? 0 : 1;
    }
    return run_installer(inst);
}
