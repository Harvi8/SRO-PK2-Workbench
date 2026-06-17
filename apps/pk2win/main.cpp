#include "resource.h"

#include "pk2/archive.h"
#include "pk2/path.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

HINSTANCE gInstance = nullptr;
HWND gMain = nullptr;
HWND gTree = nullptr;
HWND gList = nullptr;
HWND gStatus = nullptr;
HWND gProgress = nullptr;
DWORD gUiThreadId = 0;

pk2::Pk2Archive gArchive;
bool gLoaded = false;
bool gBusy = false;
std::string gCurrentFolder;

constexpr UINT WM_APP_OPEN_COMPLETE = WM_APP + 1;
constexpr UINT WM_APP_TASK_PROGRESS = WM_APP + 2;
constexpr UINT WM_APP_TASK_COMPLETE = WM_APP + 3;

struct OpenResult {
    bool ok{};
    pk2::Pk2Archive archive;
    std::wstring message;
};

struct TaskProgress {
    int percent{};
    std::wstring message;
};

struct TaskResult {
    bool ok{};
    bool archiveChanged{};
    std::string refreshFolder;
    std::wstring message;
};

HGLOBAL createDropEffectGlobal(DWORD effect) {
    auto handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
    if (handle == nullptr) {
        return nullptr;
    }
    auto* value = static_cast<DWORD*>(GlobalLock(handle));
    if (value == nullptr) {
        GlobalFree(handle);
        return nullptr;
    }
    *value = effect;
    GlobalUnlock(handle);
    return handle;
}

constexpr const wchar_t* kAppTitle = L"PK2 Workbench PRO - by kahme247";
constexpr const wchar_t* kAppTitlePrefix = L"PK2 Workbench PRO";
constexpr const wchar_t* kAppCredit = L"by kahme247";

std::wstring absolutePathWide(const fs::path& path) {
    const auto input = path.wstring();
    const auto required = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        throw pk2::Pk2Error("Could not resolve path: " + pk2::pathUtf8(path));
    }

    std::wstring result(required, L'\0');
    const auto written = GetFullPathNameW(input.c_str(),
                                          required,
                                          result.data(),
                                          nullptr);
    if (written == 0 || written >= required) {
        throw pk2::Pk2Error("Could not resolve path: " + pk2::pathUtf8(path));
    }
    result.resize(written);
    return result;
}

std::string filesystemErrorText(const std::string& action,
                                const fs::path& path,
                                const std::error_code& error) {
    return action + ": " + pk2::pathUtf8(path) + " (" + error.message() + ")";
}

void createDirectoriesChecked(const fs::path& path) {
    std::error_code error;
    fs::create_directories(path, error);
    if (error) {
        throw pk2::Pk2Error(filesystemErrorText("Could not create directory", path, error));
    }
}

bool existsChecked(const fs::path& path, const std::string& action) {
    std::error_code error;
    const auto exists = fs::exists(path, error);
    if (error) {
        throw pk2::Pk2Error(filesystemErrorText(action, path, error));
    }
    return exists;
}

bool isDirectoryChecked(const fs::path& path, const std::string& action) {
    std::error_code error;
    const auto isDirectory = fs::is_directory(path, error);
    if (error) {
        throw pk2::Pk2Error(filesystemErrorText(action, path, error));
    }
    return isDirectory;
}

bool isRegularFileChecked(const fs::path& path, const std::string& action) {
    std::error_code error;
    const auto isRegular = fs::is_regular_file(path, error);
    if (error) {
        throw pk2::Pk2Error(filesystemErrorText(action, path, error));
    }
    return isRegular;
}

HGLOBAL createHDropGlobal(const std::vector<fs::path>& paths) {
    std::vector<std::wstring> nativePaths;
    nativePaths.reserve(paths.size());
    std::size_t pathChars = 1;
    for (const auto& path : paths) {
        auto native = absolutePathWide(path);
        pathChars += native.size() + 1;
        nativePaths.push_back(std::move(native));
    }

    const auto byteCount = sizeof(DROPFILES) + pathChars * sizeof(wchar_t);
    auto handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, byteCount);
    if (handle == nullptr) {
        return nullptr;
    }

    auto* drop = static_cast<DROPFILES*>(GlobalLock(handle));
    if (drop == nullptr) {
        GlobalFree(handle);
        return nullptr;
    }

    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    auto* cursor = reinterpret_cast<wchar_t*>(
        reinterpret_cast<std::uint8_t*>(drop) + sizeof(DROPFILES));
    for (const auto& path : nativePaths) {
        const auto chars = path.size() + 1;
        CopyMemory(cursor, path.c_str(), chars * sizeof(wchar_t));
        cursor += chars;
    }
    *cursor = L'\0';
    GlobalUnlock(handle);
    return handle;
}

FORMATETC makeFormatEtc(CLIPFORMAT format) {
    FORMATETC result{};
    result.cfFormat = format;
    result.dwAspect = DVASPECT_CONTENT;
    result.lindex = -1;
    result.tymed = TYMED_HGLOBAL;
    return result;
}

class ShellDropSource final : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (iid == IID_IUnknown || iid == IID_IDropSource) {
            *object = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refCount_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const auto count = InterlockedDecrement(&refCount_);
        if (count == 0) {
            delete this;
        }
        return static_cast<ULONG>(count);
    }

    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed, DWORD keyState) override {
        if (escapePressed) {
            return DRAGDROP_S_CANCEL;
        }
        if ((keyState & MK_LBUTTON) == 0) {
            return DRAGDROP_S_DROP;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    ~ShellDropSource() = default;

    LONG refCount_{1};
};

class HdropDataObject final : public IDataObject {
public:
    explicit HdropDataObject(std::vector<fs::path> paths)
        : paths_(std::move(paths)),
          preferredDropEffect_(static_cast<CLIPFORMAT>(
              RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT))) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (iid == IID_IUnknown || iid == IID_IDataObject) {
            *object = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refCount_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const auto count = InterlockedDecrement(&refCount_);
        if (count == 0) {
            delete this;
        }
        return static_cast<ULONG>(count);
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (format == nullptr || medium == nullptr) {
            return E_POINTER;
        }
        ZeroMemory(medium, sizeof(*medium));
        if (QueryGetData(format) != S_OK) {
            return DATA_E_FORMATETC;
        }

        HGLOBAL handle = nullptr;
        if (format->cfFormat == CF_HDROP) {
            handle = createHDropGlobal(paths_);
        } else if (format->cfFormat == preferredDropEffect_) {
            handle = createDropEffectGlobal(DROPEFFECT_COPY);
        }
        if (handle == nullptr) {
            return STG_E_MEDIUMFULL;
        }

        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = handle;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override {
        return DATA_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        if (format == nullptr) {
            return E_POINTER;
        }
        if ((format->tymed & TYMED_HGLOBAL) == 0 || format->dwAspect != DVASPECT_CONTENT) {
            return DATA_E_FORMATETC;
        }
        if (format->cfFormat == CF_HDROP || format->cfFormat == preferredDropEffect_) {
            return S_OK;
        }
        return DATA_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* formatOut) override {
        if (formatOut != nullptr) {
            formatOut->ptd = nullptr;
        }
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** enumFormats) override {
        if (enumFormats == nullptr) {
            return E_POINTER;
        }
        *enumFormats = nullptr;
        if (direction != DATADIR_GET) {
            return E_NOTIMPL;
        }

        FORMATETC formats[] = {
            makeFormatEtc(CF_HDROP),
            makeFormatEtc(preferredDropEffect_),
        };
        return SHCreateStdEnumFmtEtc(_countof(formats), formats, enumFormats);
    }

    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    ~HdropDataObject() = default;

    LONG refCount_{1};
    std::vector<fs::path> paths_;
    CLIPFORMAT preferredDropEffect_{};
};

std::wstring toWide(const std::string& text) {
    try {
        return pk2::widenUtf8(text);
    } catch (...) {
        if (text.empty()) {
            return {};
        }
        const int size = MultiByteToWideChar(CP_ACP,
                                             0,
                                             text.data(),
                                             static_cast<int>(text.size()),
                                             nullptr,
                                             0);
        if (size <= 0) {
            return L"An error occurred, but Windows could not decode the message text.";
        }
        std::wstring result(static_cast<std::size_t>(size), L'\0');
        MultiByteToWideChar(CP_ACP,
                            0,
                            text.data(),
                            static_cast<int>(text.size()),
                            result.data(),
                            size);
        return result;
    }
}

std::string toUtf8(const std::wstring& text) {
    return pk2::narrowUtf8(text);
}

std::wstring safePathText(const fs::path& path) {
    try {
        return toWide(pk2::pathUtf8(path));
    } catch (...) {
        try {
            return path.wstring();
        } catch (...) {
            return L"<path unavailable>";
        }
    }
}

std::wstring exceptionMessage(const std::exception& ex) {
    if (const auto* filesystemError = dynamic_cast<const fs::filesystem_error*>(&ex)) {
        std::wstring message = L"Filesystem error";
        if (filesystemError->code()) {
            message += L": ";
            message += toWide(filesystemError->code().message());
        }
        if (!filesystemError->path1().empty()) {
            message += L"\r\nPath: ";
            message += safePathText(filesystemError->path1());
        }
        if (!filesystemError->path2().empty()) {
            message += L"\r\nTarget: ";
            message += safePathText(filesystemError->path2());
        }
        return message;
    }
    if (const auto* systemError = dynamic_cast<const std::system_error*>(&ex)) {
        std::wstring message = L"System error";
        if (systemError->code()) {
            message += L" ";
            message += std::to_wstring(systemError->code().value());
            message += L" (";
            message += toWide(systemError->code().category().name());
            message += L"): ";
            message += toWide(systemError->code().message());
        } else {
            message += L": ";
            message += toWide(ex.what());
        }
        return message;
    }
    return toWide(ex.what());
}

void setStatus(const std::wstring& text) {
    SendMessageW(gStatus, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text.c_str()));
}

void showError(const std::exception& ex) {
    const auto message = exceptionMessage(ex);
    MessageBoxW(gMain, message.c_str(), L"PK2 Tool Error", MB_ICONERROR | MB_OK);
}

std::wstring formatSize(std::uint64_t bytes) {
    static constexpr const wchar_t* kUnits[] = {L"B", L"KB", L"MB", L"GB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }

    std::wostringstream out;
    if (unit == 0) {
        out << bytes << L" " << kUnits[unit];
    } else if (value >= 100.0) {
        out << std::fixed << std::setprecision(0) << value << L" " << kUnits[unit];
    } else if (value >= 10.0) {
        out << std::fixed << std::setprecision(1) << value << L" " << kUnits[unit];
    } else {
        out << std::fixed << std::setprecision(2) << value << L" " << kUnits[unit];
    }
    return out.str();
}

void setProgressVisible(bool visible) {
    if (gProgress != nullptr) {
        ShowWindow(gProgress, visible ? SW_SHOW : SW_HIDE);
        if (!visible) {
            SendMessageW(gProgress, PBM_SETPOS, 0, 0);
        }
    }
}

void setProgress(int percent) {
    percent = std::clamp(percent, 0, 100);
    if (gProgress != nullptr) {
        SendMessageW(gProgress, PBM_SETPOS, percent, 0);
    }
}

void setBusy(bool busy, const std::wstring& statusText) {
    gBusy = busy;
    EnableWindow(gTree, busy ? FALSE : TRUE);
    EnableWindow(gList, busy ? FALSE : TRUE);
    DragAcceptFiles(gMain, busy ? FALSE : TRUE);
    setProgressVisible(busy);
    if (busy) {
        setProgress(0);
    }

    const auto menu = GetMenu(gMain);
    if (menu != nullptr) {
        const auto enabled = busy ? MF_GRAYED : MF_ENABLED;
        EnableMenuItem(menu, IDM_FILE_OPEN, MF_BYCOMMAND | enabled);
        EnableMenuItem(menu, IDM_FILE_CLOSE, MF_BYCOMMAND | enabled);
        EnableMenuItem(menu, IDM_FILE_SAVE, MF_BYCOMMAND | enabled);
        EnableMenuItem(menu, IDM_FILE_SAVE_AS, MF_BYCOMMAND | enabled);
        EnableMenuItem(menu, IDM_FILE_EXIT, MF_BYCOMMAND | enabled);
        EnableMenuItem(menu, IDM_EXTRACT_SELECTED, MF_BYCOMMAND | enabled);
        EnableMenuItem(menu, IDM_EXTRACT_SHOWN, MF_BYCOMMAND | enabled);
        EnableMenuItem(menu, IDM_IMPORT_FILE, MF_BYCOMMAND | enabled);
        EnableMenuItem(menu, IDM_IMPORT_FOLDER, MF_BYCOMMAND | enabled);
        EnableMenuItem(menu, IDM_DELETE_ENTRY, MF_BYCOMMAND | enabled);
        DrawMenuBar(gMain);
    }

    if (!statusText.empty()) {
        setStatus(statusText);
    }
}

void postProgress(HWND targetWindow,
                  std::uint64_t completed,
                  std::uint64_t total,
                  std::string_view currentPath,
                  const std::wstring& verb,
                  int& lastPercent,
                  std::chrono::steady_clock::time_point& lastPost) {
    int percent = 100;
    if (total > 0) {
        percent = static_cast<int>((completed * 100) / total);
    }

    const auto now = std::chrono::steady_clock::now();
    if (percent != 100 &&
        percent == lastPercent &&
        now - lastPost < std::chrono::milliseconds(120)) {
        return;
    }

    std::wstring message = verb;
    if (!currentPath.empty()) {
        message += L" ";
        message += toWide(std::string(currentPath));
    }
    if (total > 0) {
        message += L" (";
        message += formatSize(completed);
        message += L" / ";
        message += formatSize(total);
        message += L")";
    }

    auto update = std::make_unique<TaskProgress>();
    update->percent = percent;
    update->message = std::move(message);
    if (PostMessageW(targetWindow,
                     WM_APP_TASK_PROGRESS,
                     0,
                     reinterpret_cast<LPARAM>(update.get()))) {
        update.release();
        lastPercent = percent;
        lastPost = now;
    }
}

std::optional<fs::path> openFileDialog(const wchar_t* title,
                                       const wchar_t* filter,
                                       bool save) {
    wchar_t fileName[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = gMain;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!save) {
        ofn.Flags |= OFN_FILEMUSTEXIST;
    } else {
        ofn.Flags |= OFN_OVERWRITEPROMPT;
    }

    const auto ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!ok) {
        return std::nullopt;
    }
    return fs::path(fileName);
}

std::optional<fs::path> browseFolder(const wchar_t* title) {
    BROWSEINFOW info{};
    info.hwndOwner = gMain;
    info.lpszTitle = title;
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&info);
    if (pidl == nullptr) {
        return std::nullopt;
    }

    wchar_t path[MAX_PATH]{};
    const auto ok = SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    if (!ok) {
        return std::nullopt;
    }
    return fs::path(path);
}

INT_PTR CALLBACK passwordDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* password = reinterpret_cast<std::wstring*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
    switch (message) {
    case WM_INITDIALOG:
        SetWindowLongPtrW(dialog, GWLP_USERDATA, lParam);
        CheckDlgButton(dialog, IDC_DEFAULT_PASSWORD, BST_CHECKED);
        SetDlgItemTextW(dialog, IDC_PASSWORD_EDIT, L"169841");
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            password = reinterpret_cast<std::wstring*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
            wchar_t buffer[256]{};
            GetDlgItemTextW(dialog, IDC_PASSWORD_EDIT, buffer, 256);
            *password = buffer;
            if (IsDlgButtonChecked(dialog, IDC_DEFAULT_PASSWORD) == BST_CHECKED && password->empty()) {
                *password = L"169841";
            }
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    default:
        return FALSE;
    }
}

std::optional<std::string> askPassword() {
    std::wstring password = L"169841";
    const auto result = DialogBoxParamW(gInstance,
                                        MAKEINTRESOURCEW(IDD_PASSWORD),
                                        gMain,
                                        passwordDialogProc,
                                        reinterpret_cast<LPARAM>(&password));
    if (result != IDOK) {
        return std::nullopt;
    }
    return toUtf8(password);
}

std::wstring treeItemText(HTREEITEM item) {
    wchar_t buffer[260]{};
    TVITEMW tvItem{};
    tvItem.mask = TVIF_TEXT;
    tvItem.hItem = item;
    tvItem.pszText = buffer;
    tvItem.cchTextMax = 260;
    TreeView_GetItem(gTree, &tvItem);
    return buffer;
}

std::string treeStoredPath(HTREEITEM item) {
    TVITEMW tvItem{};
    tvItem.mask = TVIF_PARAM;
    tvItem.hItem = item;
    if (!TreeView_GetItem(gTree, &tvItem) || tvItem.lParam == 0) {
        return {};
    }
    return *reinterpret_cast<std::string*>(tvItem.lParam);
}

std::string treeItemPath(HTREEITEM item) {
    return treeStoredPath(item);
}

bool hasFolderChildren(const std::string& folderPath) {
    for (const auto& entry : gArchive.children(folderPath)) {
        if (entry.type == pk2::EntryType::Folder) {
            return true;
        }
    }
    return false;
}

std::size_t directFileCount(const std::string& folderPath) {
    std::size_t count = 0;
    for (const auto& entry : gArchive.children(folderPath)) {
        if (entry.type == pk2::EntryType::File) {
            ++count;
        }
    }
    return count;
}

std::wstring archiveSummary() {
    std::size_t files = 0;
    std::size_t folders = 0;
    for (const auto& entry : gArchive.listTree()) {
        if (entry.type == pk2::EntryType::Folder) {
            ++folders;
        } else {
            ++files;
        }
    }

    std::wostringstream summary;
    summary << L"PK2 opened. " << files << L" files";
    if (folders > 0) {
        summary << L", " << folders << L" folders";
    } else {
        summary << L", flat root archive";
    }
    summary << L".";
    return summary.str();
}

void addDummyChild(HTREEITEM parent) {
    TVINSERTSTRUCTW insert{};
    insert.hParent = parent;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT;
    insert.item.pszText = const_cast<wchar_t*>(L"");
    TreeView_InsertItem(gTree, &insert);
}

void resetViews() {
    TreeView_DeleteAllItems(gTree);
    ListView_DeleteAllItems(gList);
    gCurrentFolder.clear();
}

void insertFolderTreeItems(HTREEITEM parent, const std::string& folderPath, bool oneLevelOnly) {
    for (const auto& entry : gArchive.children(folderPath)) {
        if (entry.type != pk2::EntryType::Folder) {
            continue;
        }
        const auto name = toWide(entry.name);
        TVINSERTSTRUCTW insert{};
        insert.hParent = parent;
        insert.hInsertAfter = TVI_LAST;
        insert.item.mask = TVIF_TEXT | TVIF_PARAM;
        insert.item.pszText = const_cast<wchar_t*>(name.c_str());
        insert.item.lParam = reinterpret_cast<LPARAM>(new std::string(entry.path));
        const auto item = TreeView_InsertItem(gTree, &insert);
        if (oneLevelOnly) {
            if (hasFolderChildren(entry.path)) {
                addDummyChild(item);
            }
        } else {
            insertFolderTreeItems(item, entry.path, false);
        }
    }
}

void insertRootFilesTreeItem(HTREEITEM parent) {
    const auto rootFiles = directFileCount("");
    if (rootFiles == 0) {
        return;
    }

    std::wostringstream label;
    label << L"[root files] (" << rootFiles << L")";
    const auto text = label.str();
    TVINSERTSTRUCTW insert{};
    insert.hParent = parent;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT | TVIF_PARAM;
    insert.item.pszText = const_cast<wchar_t*>(text.c_str());
    insert.item.lParam = reinterpret_cast<LPARAM>(new std::string());
    TreeView_InsertItem(gTree, &insert);
}

void populateTree() {
    resetViews();
    TVINSERTSTRUCTW root{};
    root.hParent = TVI_ROOT;
    root.hInsertAfter = TVI_ROOT;
    root.item.mask = TVIF_TEXT | TVIF_PARAM;
    root.item.pszText = const_cast<wchar_t*>(L"[root]");
    root.item.lParam = reinterpret_cast<LPARAM>(new std::string());
    const auto rootItem = TreeView_InsertItem(gTree, &root);
    insertFolderTreeItems(rootItem, "", true);
    insertRootFilesTreeItem(rootItem);
    TreeView_Expand(gTree, rootItem, TVE_EXPAND);
    TreeView_SelectItem(gTree, rootItem);
}

void expandTreeItem(HTREEITEM item) {
    const auto child = TreeView_GetChild(gTree, item);
    if (child == nullptr || !treeItemText(child).empty()) {
        return;
    }
    TreeView_DeleteItem(gTree, child);
    insertFolderTreeItems(item, treeItemPath(item), true);
}

void populateList(const std::string& folderPath) {
    ListView_DeleteAllItems(gList);
    gCurrentFolder = folderPath;

    int row = 0;
    for (const auto& entry : gArchive.children(folderPath)) {
        const auto name = toWide(entry.name);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.pszText = const_cast<wchar_t*>(name.c_str());
        ListView_InsertItem(gList, &item);

        const auto type = entry.type == pk2::EntryType::Folder ? L"Folder" : L"File";
        ListView_SetItemText(gList, row, 1, const_cast<wchar_t*>(type));

        const auto size = entry.type == pk2::EntryType::File ? formatSize(entry.size) : L"";
        ListView_SetItemText(gList, row, 2, const_cast<wchar_t*>(size.c_str()));

        const auto path = toWide(entry.path);
        ListView_SetItemText(gList, row, 3, const_cast<wchar_t*>(path.c_str()));
        ++row;
    }
}

std::optional<std::string> selectedArchivePath() {
    const auto selectedList = ListView_GetNextItem(gList, -1, LVNI_SELECTED);
    if (selectedList >= 0) {
        wchar_t buffer[1024]{};
        ListView_GetItemText(gList, selectedList, 3, buffer, 1024);
        return toUtf8(buffer);
    }

    const auto selectedTree = TreeView_GetSelection(gTree);
    if (selectedTree != nullptr) {
        return treeItemPath(selectedTree);
    }
    return std::nullopt;
}

std::optional<std::string> archivePathForListRow(int row) {
    if (row < 0) {
        return std::nullopt;
    }
    wchar_t buffer[1024]{};
    ListView_GetItemText(gList, row, 3, buffer, 1024);
    std::string path = toUtf8(buffer);
    if (path.empty()) {
        return std::nullopt;
    }
    return path;
}

void updateTitle() {
    std::wstring title = kAppTitle;
    if (gLoaded && !gArchive.sourcePath().empty()) {
        title = kAppTitlePrefix;
        title += L" - ";
        title += gArchive.sourcePath().filename().wstring();
        if (gArchive.dirty()) {
            title += L" *";
        }
        title += L" (";
        title += kAppCredit;
        title += L")";
    }
    SetWindowTextW(gMain, title.c_str());
}

fs::path archiveNamedOutputFolder(const fs::path& destination) {
    auto stem = gArchive.sourcePath().stem().wstring();
    if (stem.empty()) {
        stem = L"PK2";
    }
    return destination / stem;
}

fs::path archivePathToFilesystemPath(const fs::path& base, const std::string& archivePath) {
    fs::path result = base;
    std::stringstream stream(pk2::normalizeArchivePath(archivePath));
    std::string part;
    while (std::getline(stream, part, '/')) {
        if (!part.empty()) {
            result /= pk2::archivePathPartToFilesystem(part);
        }
    }
    return result;
}

fs::path createDragStagingRoot() {
    wchar_t tempPath[MAX_PATH + 1]{};
    const auto length = GetTempPathW(MAX_PATH, tempPath);
    if (length == 0 || length > MAX_PATH) {
        throw pk2::Pk2Error("Could not locate the Windows temporary folder.");
    }

    const auto base = fs::path(tempPath) / L"PK2WorkbenchPRO" / L"DragOut";
    createDirectoriesChecked(base);
    for (int attempt = 0; attempt < 32; ++attempt) {
        std::wstring name = std::to_wstring(GetCurrentProcessId());
        name += L"-";
        name += std::to_wstring(GetTickCount64());
        name += L"-";
        name += std::to_wstring(attempt);
        const auto candidate = base / name;
        std::error_code error;
        if (fs::create_directory(candidate, error)) {
            return candidate;
        }
    }
    throw pk2::Pk2Error("Could not create a temporary drag extraction folder.");
}

void removeFolderQuietly(const fs::path& path) {
    std::error_code error;
    fs::remove_all(path, error);
}

void updateInlineProgress(std::uint64_t completed,
                          std::uint64_t total,
                          std::string_view currentPath,
                          const std::wstring& verb,
                          int& lastPercent,
                          std::chrono::steady_clock::time_point& lastPost) {
    int percent = 100;
    if (total > 0) {
        percent = static_cast<int>((completed * 100) / total);
    }

    const auto now = std::chrono::steady_clock::now();
    if (percent != 100 &&
        percent == lastPercent &&
        now - lastPost < std::chrono::milliseconds(120)) {
        return;
    }

    std::wstring message = verb;
    if (!currentPath.empty()) {
        message += L" ";
        message += toWide(std::string(currentPath));
    }
    if (total > 0) {
        message += L" (";
        message += formatSize(completed);
        message += L" / ";
        message += formatSize(total);
        message += L")";
    }
    setProgress(percent);
    setStatus(message);
    UpdateWindow(gProgress);
    UpdateWindow(gStatus);
    lastPercent = percent;
    lastPost = now;
}

bool beginShellCopyDrag(const std::vector<fs::path>& paths) {
    auto* dataObject = new HdropDataObject(paths);
    auto* dropSource = new ShellDropSource();
    DWORD effect = DROPEFFECT_NONE;
    const auto result = DoDragDrop(dataObject, dropSource, DROPEFFECT_COPY, &effect);
    dropSource->Release();
    dataObject->Release();

    if (result == DRAGDROP_S_CANCEL || effect == DROPEFFECT_NONE) {
        return false;
    }
    if (FAILED(result)) {
        throw pk2::Pk2Error("Windows shell drag/drop failed.");
    }
    return true;
}

void startArchiveDragOut(const std::string& archivePath) {
    if (gBusy) {
        setStatus(L"Please wait for the current operation to finish.");
        return;
    }
    if (!gLoaded) {
        setStatus(L"No PK2 loaded.");
        return;
    }
    if (archivePath.empty()) {
        setStatus(L"Drag a file or folder inside the PK2, not [root].");
        return;
    }
    if (!gArchive.find(archivePath)) {
        setStatus(L"Selected archive entry was not found.");
        return;
    }

    fs::path stagingRoot;
    try {
        stagingRoot = createDragStagingRoot();
        setBusy(true, L"Preparing drag extract...");
        int lastPercent = -1;
        auto lastPost = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        auto progress = [&](std::uint64_t completed,
                            std::uint64_t total,
                            std::string_view currentPath) {
            if (GetCurrentThreadId() != gUiThreadId) {
                return;
            }
            updateInlineProgress(completed,
                                 total,
                                 currentPath,
                                 L"Preparing drag extract",
                                 lastPercent,
                                 lastPost);
        };

        gArchive.extract(archivePath,
                         stagingRoot,
                         true,
                         pk2::OverwritePolicy::Replace,
                         progress);

        const auto stagedPath = archivePathToFilesystemPath(stagingRoot, archivePath);
        if (!existsChecked(stagedPath, "Could not inspect staged drag file")) {
            throw pk2::Pk2Error("The selected archive entry was not staged for dragging.");
        }

        setProgress(100);
        setBusy(false, L"Drop into a folder to copy the extracted item.");
        const auto dropped = beginShellCopyDrag({stagedPath});
        if (dropped) {
            setStatus(L"Drag extract complete.");
        } else {
            removeFolderQuietly(stagingRoot);
            setStatus(L"Drag cancelled.");
        }
    } catch (const std::exception& ex) {
        setBusy(false, L"");
        if (!stagingRoot.empty()) {
            removeFolderQuietly(stagingRoot);
        }
        showError(ex);
    }
}

std::string validRefreshFolder(const std::string& folderPath) {
    if (folderPath.empty()) {
        return {};
    }
    const auto info = gArchive.find(folderPath);
    if (info && info->type == pk2::EntryType::Folder) {
        return folderPath;
    }
    return {};
}

void postTaskComplete(HWND targetWindow, std::unique_ptr<TaskResult> result) {
    if (PostMessageW(targetWindow,
                     WM_APP_TASK_COMPLETE,
                     0,
                     reinterpret_cast<LPARAM>(result.get()))) {
        result.release();
    }
}

void startExtraction(const std::string& archivePath,
                     const fs::path& destination,
                     bool recurse) {
    setBusy(true, L"Extracting...");
    const auto targetWindow = gMain;
    std::thread([archivePath, destination, recurse, targetWindow]() {
        auto result = std::make_unique<TaskResult>();
        int lastPercent = -1;
        auto lastPost = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        auto progress = [targetWindow, &lastPercent, &lastPost](
                            std::uint64_t completed,
                            std::uint64_t total,
                            std::string_view currentPath) {
            postProgress(targetWindow,
                         completed,
                         total,
                         currentPath,
                         L"Extracting",
                         lastPercent,
                         lastPost);
        };

        try {
            gArchive.extract(archivePath,
                             destination,
                             recurse,
                             pk2::OverwritePolicy::Replace,
                             progress);
            result->ok = true;
            result->message = L"Extraction complete.";
        } catch (const std::exception& ex) {
            result->ok = false;
            result->message = exceptionMessage(ex);
        }
        postTaskComplete(targetWindow, std::move(result));
    }).detach();
}

void startImportPaths(std::vector<fs::path> paths, std::string targetFolder) {
    if (paths.empty()) {
        return;
    }

    setBusy(true, L"Importing...");
    const auto targetWindow = gMain;
    std::thread([paths = std::move(paths), targetFolder = std::move(targetFolder), targetWindow]() {
        auto result = std::make_unique<TaskResult>();
        int lastPercent = -1;
        auto lastPost = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        auto progress = [targetWindow, &lastPercent, &lastPost](
                            std::uint64_t completed,
                            std::uint64_t total,
                            std::string_view currentPath) {
            postProgress(targetWindow,
                         completed,
                         total,
                         currentPath,
                         L"Importing",
                         lastPercent,
                         lastPost);
        };

        try {
            for (const auto& path : paths) {
                const auto target = pk2::joinArchivePath(targetFolder, pk2::pathFileNameUtf8(path));
                if (isDirectoryChecked(path, "Could not inspect dropped path")) {
                    gArchive.importFolder(path, target, progress);
                } else if (isRegularFileChecked(path, "Could not inspect dropped path")) {
                    gArchive.importFile(path, target, progress);
                } else {
                    throw pk2::Pk2Error("Dropped path is not a file or folder: " +
                                        pk2::pathUtf8(path));
                }
            }
            result->ok = true;
            result->archiveChanged = true;
            result->refreshFolder = targetFolder;
            result->message = paths.size() == 1 ? L"Import complete." : L"Imports complete.";
        } catch (const std::exception& ex) {
            result->ok = false;
            result->archiveChanged = true;
            result->refreshFolder = targetFolder;
            result->message = exceptionMessage(ex);
        }
        postTaskComplete(targetWindow, std::move(result));
    }).detach();
}

void openArchive() {
    if (gBusy) {
        setStatus(L"Please wait for the current operation to finish.");
        return;
    }
    try {
        const auto file = openFileDialog(L"Open PK2", L"PK2 files (*.pk2)\0*.pk2\0All files\0*.*\0", false);
        if (!file) {
            return;
        }
        const auto password = askPassword();
        if (!password) {
            return;
        }
        resetViews();
        setBusy(true, L"Loading PK2 metadata...");

        const auto archivePath = *file;
        const auto archivePassword = *password;
        const auto targetWindow = gMain;
        std::thread([archivePath, archivePassword, targetWindow]() {
            auto result = std::make_unique<OpenResult>();
            try {
                result->archive = pk2::Pk2Archive::open(archivePath, archivePassword);
                result->ok = true;
                result->message = L"PK2 opened.";
            } catch (const std::exception& ex) {
                result->ok = false;
                result->message = exceptionMessage(ex);
            }

            if (!PostMessageW(targetWindow,
                              WM_APP_OPEN_COMPLETE,
                              0,
                              reinterpret_cast<LPARAM>(result.get()))) {
                return;
            }
            result.release();
        }).detach();
    } catch (const std::exception& ex) {
        setBusy(false, L"");
        showError(ex);
    }
}

void closeArchive() {
    if (gBusy) {
        setStatus(L"Please wait for the current operation to finish.");
        return;
    }
    gArchive = pk2::Pk2Archive();
    gLoaded = false;
    resetViews();
    setStatus(L"No PK2 loaded.");
    updateTitle();
}

void saveArchive(bool saveAs) {
    if (gBusy) {
        setStatus(L"Please wait for the current operation to finish.");
        return;
    }
    if (!gLoaded) {
        setStatus(L"No PK2 loaded.");
        return;
    }
    try {
        if (saveAs || gArchive.sourcePath().empty()) {
            const auto file = openFileDialog(L"Save PK2 As", L"PK2 files (*.pk2)\0*.pk2\0All files\0*.*\0", true);
            if (!file) {
                return;
            }
            gArchive.saveAs(*file);
        } else {
            gArchive.save();
        }
        setStatus(L"PK2 saved.");
        updateTitle();
    } catch (const std::exception& ex) {
        showError(ex);
    }
}

void extractPath(const std::string& path, bool recurse, bool wrapInArchiveFolder) {
    if (gBusy) {
        setStatus(L"Please wait for the current operation to finish.");
        return;
    }
    if (!gLoaded) {
        setStatus(L"No PK2 loaded.");
        return;
    }
    try {
        const auto folder = browseFolder(L"Choose extract destination");
        if (!folder) {
            return;
        }
        const auto destination = wrapInArchiveFolder ? archiveNamedOutputFolder(*folder) : *folder;
        startExtraction(path, destination, recurse);
    } catch (const std::exception& ex) {
        showError(ex);
    }
}

void importFile() {
    if (gBusy) {
        setStatus(L"Please wait for the current operation to finish.");
        return;
    }
    if (!gLoaded) {
        setStatus(L"No PK2 loaded.");
        return;
    }
    try {
        const auto file = openFileDialog(L"Import File", L"All files\0*.*\0", false);
        if (!file) {
            return;
        }
        startImportPaths({*file}, gCurrentFolder);
    } catch (const std::exception& ex) {
        showError(ex);
    }
}

void importFolder() {
    if (gBusy) {
        setStatus(L"Please wait for the current operation to finish.");
        return;
    }
    if (!gLoaded) {
        setStatus(L"No PK2 loaded.");
        return;
    }
    try {
        const auto folder = browseFolder(L"Choose folder to import");
        if (!folder) {
            return;
        }
        startImportPaths({*folder}, gCurrentFolder);
    } catch (const std::exception& ex) {
        showError(ex);
    }
}

void deleteSelected() {
    if (gBusy) {
        setStatus(L"Please wait for the current operation to finish.");
        return;
    }
    if (!gLoaded) {
        setStatus(L"No PK2 loaded.");
        return;
    }
    const auto path = selectedArchivePath();
    if (!path || path->empty()) {
        setStatus(L"Select an entry first.");
        return;
    }
    std::wstring question = L"Delete \"";
    question += toWide(*path);
    question += L"\" from the PK2?";
    if (MessageBoxW(gMain, question.c_str(), L"Delete Entry", MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }
    try {
        gArchive.deleteEntry(*path);
        populateTree();
        populateList(gCurrentFolder);
        setStatus(L"Entry deleted.");
        updateTitle();
    } catch (const std::exception& ex) {
        showError(ex);
    }
}

void showMd5Helper() {
    const auto password = askPassword();
    if (!password) {
        return;
    }
    const auto digest = pk2::md5Hex(*password);
    std::wstring message = L"MD5:\r\n";
    message += toWide(digest);
    MessageBoxW(gMain, message.c_str(), L"Private-server MD5 Helper", MB_OK | MB_ICONINFORMATION);
}

void showAbout() {
    std::wstring message = L"PK2 Workbench PRO\r\n";
    message += kAppCredit;
    message += L"\r\n\r\nAll-in-one PK2 editor, extractor, importer, and archive browser.";
    MessageBoxW(gMain, message.c_str(), L"About PK2 Workbench PRO", MB_OK | MB_ICONINFORMATION);
}

void createControls(HWND window) {
    gTree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | TVS_HASLINES | TVS_HASBUTTONS | TVS_LINESATROOT,
                            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_TREE), gInstance, nullptr);
    gList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_LIST), gInstance, nullptr);
    gStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
                              WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                              window, reinterpret_cast<HMENU>(IDC_STATUS), gInstance, nullptr);
    gProgress = CreateWindowExW(0, PROGRESS_CLASSW, L"",
                                WS_CHILD | PBS_SMOOTH,
                                0, 0, 0, 0,
                                gStatus,
                                reinterpret_cast<HMENU>(IDC_PROGRESS),
                                gInstance,
                                nullptr);
    SendMessageW(gProgress, PBM_SETRANGE32, 0, 100);

    ListView_SetExtendedListViewStyle(gList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.pszText = const_cast<wchar_t*>(L"Name");
    column.cx = 260;
    ListView_InsertColumn(gList, 0, &column);
    column.pszText = const_cast<wchar_t*>(L"Type");
    column.cx = 80;
    ListView_InsertColumn(gList, 1, &column);
    column.pszText = const_cast<wchar_t*>(L"Size");
    column.cx = 120;
    ListView_InsertColumn(gList, 2, &column);
    column.pszText = const_cast<wchar_t*>(L"Path");
    column.cx = 320;
    ListView_InsertColumn(gList, 3, &column);
}

void layoutControls(HWND window) {
    RECT rect{};
    GetClientRect(window, &rect);
    SendMessageW(gStatus, WM_SIZE, 0, 0);

    RECT statusRect{};
    GetWindowRect(gStatus, &statusRect);
    const auto statusHeight = statusRect.bottom - statusRect.top;
    const auto width = rect.right - rect.left;
    const auto height = rect.bottom - rect.top - statusHeight;
    const auto treeWidth = width / 3;

    const LONG progressWidth = 260;
    const auto statusTextWidth = std::max<LONG>(0, width - progressWidth - 8);
    int statusParts[2] = {static_cast<int>(statusTextWidth), -1};
    SendMessageW(gStatus, SB_SETPARTS, 2, reinterpret_cast<LPARAM>(statusParts));
    RECT progressRect{};
    SendMessageW(gStatus, SB_GETRECT, 1, reinterpret_cast<LPARAM>(&progressRect));
    MoveWindow(gProgress,
               progressRect.left + 4,
               progressRect.top + 3,
               std::max<LONG>(0, progressRect.right - progressRect.left - 8),
               std::max<LONG>(0, progressRect.bottom - progressRect.top - 6),
               TRUE);

    MoveWindow(gTree, 0, 0, treeWidth, height, TRUE);
    MoveWindow(gList, treeWidth, 0, width - treeWidth, height, TRUE);
}

void handleNotify(LPARAM lParam) {
    const auto* header = reinterpret_cast<NMHDR*>(lParam);
    if (header->hwndFrom == gTree && header->code == TVN_DELETEITEMW) {
        const auto* change = reinterpret_cast<NMTREEVIEWW*>(lParam);
        if (change->itemOld.lParam != 0) {
            delete reinterpret_cast<std::string*>(change->itemOld.lParam);
        }
        return;
    }
    if (header->hwndFrom == gTree && header->code == TVN_ITEMEXPANDINGW) {
        const auto* change = reinterpret_cast<NMTREEVIEWW*>(lParam);
        if (change->action == TVE_EXPAND) {
            expandTreeItem(change->itemNew.hItem);
        }
        return;
    }
    if (header->hwndFrom == gTree && header->code == TVN_BEGINDRAGW) {
        const auto* change = reinterpret_cast<NMTREEVIEWW*>(lParam);
        TreeView_SelectItem(gTree, change->itemNew.hItem);
        startArchiveDragOut(treeItemPath(change->itemNew.hItem));
        return;
    }
    if (header->hwndFrom == gTree && header->code == TVN_SELCHANGEDW) {
        const auto* change = reinterpret_cast<NMTREEVIEWW*>(lParam);
        populateList(treeItemPath(change->itemNew.hItem));
        return;
    }

    if (header->hwndFrom == gList && header->code == LVN_BEGINDRAG) {
        const auto* drag = reinterpret_cast<NMLISTVIEW*>(lParam);
        ListView_SetItemState(gList, drag->iItem, LVIS_SELECTED, LVIS_SELECTED);
        if (const auto path = archivePathForListRow(drag->iItem)) {
            startArchiveDragOut(*path);
        }
        return;
    }

    if (header->hwndFrom == gList && header->code == NM_DBLCLK) {
        const auto selected = selectedArchivePath();
        if (!selected) {
            return;
        }
        const auto info = gArchive.find(*selected);
        if (info && info->type == pk2::EntryType::Folder) {
            populateList(*selected);
        }
    }
}

void handleDropFiles(HDROP drop) {
    std::vector<fs::path> paths;
    const auto count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        const auto length = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring path(length + 1, L'\0');
        DragQueryFileW(drop, i, path.data(), length + 1);
        path.resize(length);
        paths.emplace_back(path);
    }
    DragFinish(drop);

    if (gBusy) {
        setStatus(L"Please wait for the current operation to finish.");
        return;
    }
    if (!gLoaded) {
        setStatus(L"Open a PK2 before dropping files.");
        return;
    }
    startImportPaths(std::move(paths), gCurrentFolder);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        createControls(window);
        DragAcceptFiles(window, TRUE);
        setStatus(L"No PK2 loaded.");
        return 0;
    case WM_SIZE:
        layoutControls(window);
        return 0;
    case WM_NOTIFY:
        handleNotify(lParam);
        return 0;
    case WM_COMMAND:
        if (gBusy) {
            setStatus(L"Please wait for the current operation to finish.");
            return 0;
        }
        switch (LOWORD(wParam)) {
        case IDM_FILE_OPEN:
            openArchive();
            break;
        case IDM_FILE_CLOSE:
            closeArchive();
            break;
        case IDM_FILE_SAVE:
            saveArchive(false);
            break;
        case IDM_FILE_SAVE_AS:
            saveArchive(true);
            break;
        case IDM_FILE_EXIT:
            DestroyWindow(window);
            break;
        case IDM_EXTRACT_SELECTED:
            if (const auto selected = selectedArchivePath()) {
                extractPath(*selected, true, false);
            }
            break;
        case IDM_EXTRACT_SHOWN:
            extractPath(gCurrentFolder, true, true);
            break;
        case IDM_IMPORT_FILE:
            importFile();
            break;
        case IDM_IMPORT_FOLDER:
            importFolder();
            break;
        case IDM_DELETE_ENTRY:
            deleteSelected();
            break;
        case IDM_HELP_MD5:
            showMd5Helper();
            break;
        case IDM_HELP_ABOUT:
            showAbout();
            break;
        default:
            break;
        }
        return 0;
    case WM_DROPFILES:
        handleDropFiles(reinterpret_cast<HDROP>(wParam));
        return 0;
    case WM_CLOSE:
        if (gBusy) {
            setStatus(L"Please wait for the current operation to finish.");
            return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_APP_OPEN_COMPLETE: {
        std::unique_ptr<OpenResult> result(reinterpret_cast<OpenResult*>(lParam));
        setBusy(false, L"");
        if (result->ok) {
            gArchive = std::move(result->archive);
            gLoaded = true;
            populateTree();
            populateList("");
            setStatus(archiveSummary());
            updateTitle();
        } else {
            gLoaded = false;
            resetViews();
            setStatus(L"PK2 load failed.");
            MessageBoxW(gMain, result->message.c_str(), L"PK2 Tool Error", MB_ICONERROR | MB_OK);
            updateTitle();
        }
        return 0;
    }
    case WM_APP_TASK_PROGRESS: {
        std::unique_ptr<TaskProgress> progress(reinterpret_cast<TaskProgress*>(lParam));
        setProgress(progress->percent);
        setStatus(progress->message);
        return 0;
    }
    case WM_APP_TASK_COMPLETE: {
        std::unique_ptr<TaskResult> result(reinterpret_cast<TaskResult*>(lParam));
        setBusy(false, L"");
        if (result->ok) {
            if (result->archiveChanged) {
                const auto refreshFolder = validRefreshFolder(result->refreshFolder);
                populateTree();
                populateList(refreshFolder);
                updateTitle();
            }
            setStatus(result->message);
        } else {
            if (result->archiveChanged) {
                const auto refreshFolder = validRefreshFolder(result->refreshFolder);
                populateTree();
                populateList(refreshFolder);
                updateTitle();
            }
            setStatus(L"Operation failed.");
            MessageBoxW(gMain, result->message.c_str(), L"PK2 Tool Error", MB_ICONERROR | MB_OK);
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand) {
    gInstance = instance;
    gUiThreadId = GetCurrentThreadId();

    const auto oleResult = OleInitialize(nullptr);
    if (FAILED(oleResult)) {
        return 1;
    }

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&controls);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.lpszClassName = L"PK2WorkbenchPROWindow";
    wc.lpfnWndProc = windowProc;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszMenuName = MAKEINTRESOURCEW(IDR_MAIN_MENU);

    if (!RegisterClassExW(&wc)) {
        OleUninitialize();
        return 1;
    }

    gMain = CreateWindowExW(0,
                            wc.lpszClassName,
                            kAppTitle,
                            WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT,
                            CW_USEDEFAULT,
                            1000,
                            650,
                            nullptr,
                            nullptr,
                            instance,
                            nullptr);
    if (gMain == nullptr) {
        OleUninitialize();
        return 1;
    }

    ShowWindow(gMain, showCommand);
    UpdateWindow(gMain);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    const auto exitCode = static_cast<int>(message.wParam);
    OleUninitialize();
    return exitCode;
}
