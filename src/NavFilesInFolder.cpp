/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/DirScan.h"
#include "gui/Dpi.h"
#include "base/File.h"
#include "base/GuessFileType.h"
#include "base/Timer.h"
#include "base/UITask.h"
#include "base/Win.h"

#include "gui/UIModels.h"
#include "gui/Layout.h"
#include "gui/win/WinGui.h"
#include "gui/PlatformFont.h"
#include "gui/Gfx.h"
#include "gui/GuiColors.h"
#include "gui/VirtCtrl.h"

#include "Settings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "FileHistory.h"
#include "SumatraConfig.h"
#include "AppSettings.h"
#include "MainWindow.h"
#include "AIChatTypes.h"
#include "WindowTab.h"
#include "SumatraPDF.h"
#include "Theme.h"
#include "Translations.h"
#include "DarkMode.h"
#include "SvgIcons.h"
#include "FilterUtil.h"
#include "FilterHighlightDraw.h"
#include "NavFilesInFolder.h"
#include "NavFilesInFolderCommon.h"
#include "AppHelpersCommon.h"
#include "Menu.h"
#include "PagePosition.h"

// A modeless directory browser listing sub-directories and files SumatraPDF
// can open (judged by extension). Enter / double-click replaces the document
// in the current tab or descends into a directory; Ctrl + Enter / Ctrl +
// double-click switches to the tab already showing the file or opens it in a
// new tab; ".." goes one directory up. Back / Forward walk the folders visited
// in this session, Up goes to the parent, Home lists the drives and Explorer's
// Quick access. The window stays open so you can open several files in
// succession; Esc or the close button dismisses it.

enum NavBtn {
    NavBtnBack,
    NavBtnForward,
    NavBtnUp,
    NavBtnHome,
    NavBtnCount
};

// Clear: show ".." alone until the listing arrives (new folder).
// Keep: leave the current listing on screen while it is re-read (F5,
// activation); the result replaces it only if something changed
enum class NavListReset {
    Clear,
    Keep
};

// how in-place editing of the path ends: Enter navigates, Esc restores
enum class NavPathEditEnd {
    Commit,
    Cancel
};

// logical (pre-DPI) sizes for placement / sizing of the nav window
constexpr int kNavDockMinFreeDx = 320;  // free strip beside main must be wider than this to dock
constexpr int kNavDockMaxWidthDx = 480; // docked outer width = min(this, free strip)
constexpr int kNavMinClientDx = 200;    // floor after subtracting window chrome
constexpr int kNavMinClientDy = 200;
constexpr int kNavFallbackMinDy = 480; // centered (non-docked) client size
constexpr int kNavFallbackMinDx = 480;
constexpr int kNavFallbackMaxDx = 720;
constexpr int kNavFallbackMainDxMargin = 256; // main client dx minus this → preferred width
constexpr int kNavFallbackMainDyMargin = 72;
constexpr int kNavFallbackYOffset = 42; // top offset when centered over main

struct ListBoxModelNav : ListBoxModel {
    Vec<NavFileEntry> all;     // owned; the whole listing
    Vec<NavFileEntry> entries; // shown: the entries of `all` that pass the filter

    ~ListBoxModelNav() override {
        for (NavFileEntry& e : all) {
            FreeNavEntry(e);
        }
    }
    int ItemsCount() override { return len(entries); }
    Str Item(int i) override { return entries[i].name; }
};

struct NavFilesInFolderWnd : WindowBase {
    ~NavFilesInFolderWnd() override;

    MainWindow* win = nullptr;
    // the label, the buttons, the list and the hints are virtual controls
    VirtText* dirLabel = nullptr;
    Edit* dirEdit = nullptr; // HWND; shown over dirLabel while editing the path
    bool editingPath = false;
    VirtIconButton* navBtns[NavBtnCount]{};
    Edit* filterEdit = nullptr; // HWND
    StrVec filterWords;
    Vec<u8> highlighted; // scratch for DrawMaybeHighlightedText
    VirtListBox* listBox = nullptr;
    Str currDir; // owned; empty in the home view
    int scanGen = 0;
    bool scanInFlight = false;
    Str pendingSelectPath; // owned; file to select when a scan finishes
    int pendingSelectIdx = -1;
    bool skipHistory = false;

    void OnKeyDown(KeyEvent* ev);
    void OnActivate(WindowBase::ActivateEvent* ev);
    void OnFocus(WindowBase::FocusEvent* ev);

    bool Create(MainWindow* win, Str filePath);
    void CreateNavButtons(Color fg, Color bg);
    bool IsHome() const;
    void SetDir(Str dir, Str selectPath, int selectIdx = -1, NavListReset reset = NavListReset::Clear);
    void Navigate(Str dir, Str selectPath = {});
    TempStr SelectedPathTemp();
    void RefreshList();
    void ExecuteCurrentSelection(bool inNewTab = false);
    void DeleteCurrentSelection();
    void OnListDoubleClick();
    void GoUp();
    void GoBack();
    void GoForward();
    void GoHome();
    void OnFilterChanged();
    void ApplyFilter();
    void ClearFilter();
    void OnListChar(VirtCharEvent* ev);
    void OnDirLabelClick(VirtMouseEvent* ev);
    void OnDirEditKillFocus();
    void BeginEditPath();
    void EndEditPath(NavPathEditEnd how);
    Rect PathEditRect();
    void DrawListBoxItem(VirtListBox::DrawItemEvent* ev);
    void UpdateDirLabel();
    void UpdateNavButtons();
};

static NavFilesInFolderWnd* gNavFilesWnd = nullptr;
static HWND gHwndToActivateOnNavClose = nullptr;

NavFilesInFolderWnd::~NavFilesInFolderWnd() {
    scanGen++; // in-flight scans must not apply to a destroyed window
    str::Free(currDir);
    str::Free(pendingSelectPath);
}

static void SafeDeleteNavFilesWnd() {
    if (!gNavFilesWnd) {
        return;
    }
    auto* tmp = gNavFilesWnd;
    gNavFilesWnd = nullptr;
    delete tmp;
    if (gHwndToActivateOnNavClose) {
        HWND fg = GetForegroundWindow();
        if (!fg || fg == gHwndToActivateOnNavClose) {
            SetActiveWindow(gHwndToActivateOnNavClose);
        }
        gHwndToActivateOnNavClose = nullptr;
    }
}

static void ScheduleDeleteNavFilesWnd() {
    if (!gNavFilesWnd) {
        return;
    }
    auto fn = MkFunc0Void(SafeDeleteNavFilesWnd);
    uitask::Post(fn, "SafeDeleteNavFilesWnd");
}

// Alt-Tab / activation finishes setting focus after WM_ACTIVATE, so set list
// focus after the message queue drains (and only while this window is still active).
static void FocusNavListBox() {
    NavFilesInFolderWnd* wnd = gNavFilesWnd;
    if (!wnd || !wnd->hwnd || !IsWindow(wnd->hwnd) || !wnd->listBox) {
        return;
    }
    if (GetForegroundWindow() != wnd->hwnd) {
        return;
    }
    wnd->SetFocusTo(wnd->listBox);
}

static void ScheduleFocusNavListBox() {
    if (!gNavFilesWnd) {
        return;
    }
    auto fn = MkFunc0Void(FocusNavListBox);
    uitask::Post(fn, "FocusNavListBox");
}

// rebuild the shown entries: all of them without a filter, else those whose
// name has every filter word (the command palette's matching), without ".."
static void FilterNavEntries(ListBoxModelNav* m, const StrVec& words) {
    VecReset(m->entries);
    bool filtering = len(words) > 0;
    for (NavFileEntry& e : m->all) {
        if (filtering && str::Eq(e.name, StrL(".."))) {
            continue;
        }
        if (filtering && !FilterMatches(NavEntryBaseName(e), words)) {
            continue;
        }
        VecAppend(m->entries, e);
    }
}

static void ClearNavModel(ListBoxModelNav* m) {
    FreeNavEntries(m->all);
    VecReset(m->entries);
}

void AppendHomeFileEntry(Vec<NavFileEntry>& out, Str path) {
    if (!CanOpenFile(path)) {
        return;
    }
    // Quick access keeps listing files after they are deleted
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!path::GetCachedAttributesEx(path, &fad)) {
        return;
    }
    NavFileEntry e;
    e.path = str::Dup(path);
    e.name = str::Dup(path);
    e.size = ((i64)fad.nFileSizeHigh << 32) | (i64)fad.nFileSizeLow;
    VecAppend(out, e);
}

// Built on a worker thread: listing + filtering + sorting a folder of tens of
// thousands of files must not freeze the UI (discussion #6014).
void CollectNavEntriesForDir(Str dir, Vec<NavFileEntry>& out) {
    if (len(dir) == 0) {
        CollectHomeEntries(out);
        return;
    }
    // ".." also in a drive root, where it leads to the home view
    AppendNavParentEntry(out);
    int firstIdx = 1; // keep ".." at the top when sorting

    DirIter di{dir};
    di.includeFiles = true;
    di.includeDirs = true;
    for (DirIterEntry* de : di) {
        DWORD attrs = de->fd->dwFileAttributes;
        if (attrs & FILE_ATTRIBUTE_HIDDEN) {
            continue;
        }
        TempStr leaf = NavLeafNameTemp(de);
        if (len(leaf) == 0) {
            continue;
        }
        // own path/name before any further temp allocations
        Str fullPath = str::Dup(de->filePath);

        NavFileEntry e;
        e.path = fullPath;
        if (IsDirectory(de)) {
            e.isDir = true;
            e.name = str::Join(leaf, StrL("\\"));
        } else {
            if (!CanOpenFile(leaf)) {
                str::Free(fullPath);
                continue;
            }
            e.name = str::Dup(leaf);
            e.size = GetFileSize(de);
            // FindFirstFile size is sometimes 0 on network/cloud providers even
            // when the file has content; attributes are cached for network paths.
            if (e.size == 0) {
                WIN32_FILE_ATTRIBUTE_DATA fad{};
                if (path::GetCachedAttributesEx(fullPath, &fad) && !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    e.size = ((i64)fad.nFileSizeHigh << 32) | (i64)fad.nFileSizeLow;
                }
            }
        }
        VecAppend(out, e);
    }

    SortNavEntries(out, firstIdx);
}

static int FindEntryIndex(NavFilesInFolderWnd* wnd, ListBoxModelNav* m, Str selectPath);
static void SelectAndEnsureVisible(VirtListBox* lb, int idx);

struct NavDirScanReq {
    NavFilesInFolderWnd* wnd = nullptr;
    int gen = 0;
    bool isRefresh = false; // re-read of the dir already shown
    Str dir;                // owned
    ~NavDirScanReq() { str::Free(dir); }
};

struct NavDirScanResult {
    NavFilesInFolderWnd* wnd = nullptr;
    int gen = 0;
    bool isRefresh = false;
    Str dir; // owned
    Vec<NavFileEntry> entries;
    double totalMs = 0;
    ~NavDirScanResult() {
        str::Free(dir);
        for (NavFileEntry& e : entries) {
            FreeNavEntry(e);
        }
    }
};

static void FinishNavDirScan(NavDirScanResult* r) {
    AutoDelete del(r);
    NavFilesInFolderWnd* wnd = gNavFilesWnd;
    if (!wnd || wnd != r->wnd || r->gen != wnd->scanGen || !wnd->listBox) {
        logf("NavDirScan: drop stale gen %d (current wnd gen=%d)\n", r->gen, wnd ? wnd->scanGen : -1);
        return;
    }
    logf("NavDirScan: apply %d entries for %s (%.1fms UI thread=%d)\n", len(r->entries), r->dir, r->totalMs,
         (int)uitask::IsMainUIThread());

    auto* m = (ListBoxModelNav*)wnd->listBox->model;
    if (!m) {
        m = new ListBoxModelNav();
    }
    wnd->scanInFlight = false;
    // a re-read that found nothing new leaves the list alone: no repaint at all
    if (r->isRefresh && SameNavEntries(m->all, r->entries)) {
        return;
    }
    int scrollY = wnd->listBox->scrollY;
    StealNavEntries(m->all, r->entries);
    FilterNavEntries(m, wnd->filterWords);
    wnd->listBox->SetModel(m);
    wnd->UpdateDirLabel();

    int selIdx = 0;
    if (len(wnd->pendingSelectPath) > 0) {
        selIdx = FindEntryIndex(wnd, m, wnd->pendingSelectPath);
    } else if (wnd->pendingSelectIdx >= 0) {
        selIdx = wnd->pendingSelectIdx;
    }
    if (m->ItemsCount() > 0 && r->isRefresh) {
        // the user is looking at this list: keep the viewport where it was
        selIdx = std::min(selIdx, m->ItemsCount() - 1);
        wnd->listBox->SetCurrentSelection(selIdx);
        wnd->listBox->ScrollTo(scrollY);
    } else if (m->ItemsCount() > 0) {
        SelectAndEnsureVisible(wnd->listBox, selIdx);
    }
    wnd->listBox->Invalidate();
}

static void NavDirScanThread(NavDirScanReq* req) {
    AutoDelete delReq(req);
    auto tAll = TimeGet();
    // the home view enumerates a shell folder, which needs COM on this thread
    bool comInited = false;
    if (len(req->dir) == 0) {
        comInited = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    }
    auto* r = new NavDirScanResult;
    r->wnd = req->wnd;
    r->gen = req->gen;
    r->isRefresh = req->isRefresh;
    r->dir = str::Dup(req->dir);
    CollectNavEntriesForDir(req->dir, r->entries);
    if (comInited) {
        CoUninitialize();
    }
    r->totalMs = TimeSinceInMs(tAll);
    logf("NavDirScan: thread done dir=%s n=%d total=%.1fms UI thread=%d\n", r->dir, len(r->entries), r->totalMs,
         (int)uitask::IsMainUIThread());
    auto fn = MkFunc0(FinishNavDirScan, r);
    uitask::Post(fn, "FinishNavDirScan");
}

// full path for entry e under currDir
static TempStr NavEntryPathTemp(NavFilesInFolderWnd* wnd, NavFileEntry& e) {
    if (str::Eq(e.name, StrL(".."))) {
        return path::GetDirTemp(wnd->currDir);
    }
    if (e.path) {
        return e.path;
    }
    Str name = e.name;
    if (e.isDir) {
        name = Str(name.s, name.len - 1); // strip the trailing "\\"
    }
    return path::JoinTemp(wnd->currDir, name);
}

// index of selectPath in the listing, or 0 if not found / empty
static int FindEntryIndex(NavFilesInFolderWnd* wnd, ListBoxModelNav* m, Str selectPath) {
    if (!m || len(selectPath) == 0) {
        return 0;
    }
    for (int i = 0; i < len(m->entries); i++) {
        TempStr path = NavEntryPathTemp(wnd, m->entries[i]);
        if (str::EqI(path, selectPath) || path::IsSame(path, selectPath)) {
            return i;
        }
    }
    // basename fallback (path form differences: long-path prefix, slash style, etc.)
    TempStr base = path::GetBaseNameTemp(selectPath);
    for (int i = 0; i < len(m->entries); i++) {
        if (str::EqI(NavEntryBaseName(m->entries[i]), base)) {
            return i;
        }
    }
    return 0;
}

// select idx and scroll so it is visible (centered when possible)
static void SelectAndEnsureVisible(VirtListBox* lb, int idx) {
    if (!lb || idx < 0) {
        return;
    }
    int n = lb->ItemsCount();
    if (n <= 0) {
        return;
    }
    if (idx >= n) {
        idx = n - 1;
    }
    lb->SetCurrentSelection(idx);

    int itemH = lb->GetItemHeight();
    int visible = std::max(lb->UsableDy() / itemH, 1);
    int top = ClampI(idx - (visible / 2), 0, std::max(n - visible, 0));
    lb->ScrollTo(top * itemH);
}

void NavFilesInFolderWnd::UpdateDirLabel() {
    dirLabel->SetText(IsHome() ? Tr("Home") : currDir);
    dirLabel->Invalidate(); // SetText() doesn't repaint
}

void NavFilesInFolderWnd::UpdateNavButtons() {
    bool enabled[NavBtnCount] = {gNavHistIdx > 0, gNavHistIdx + 1 < len(gNavHistory), !IsHome(), !IsHome()};
    for (int i = 0; i < NavBtnCount; i++) {
        if (navBtns[i]) {
            navBtns[i]->SetIsEnabled(enabled[i]);
        }
    }
}

bool NavFilesInFolderWnd::IsHome() const {
    return len(currDir) == 0;
}

void NavFilesInFolderWnd::OnFilterChanged() {
    TempStr s = filterEdit->GetTextTemp();
    filterWords.Reset();
    SplitFilterToWords(s, filterWords);
    ApplyFilter();
}

// re-filter the current listing; a filter selects its first match
void NavFilesInFolderWnd::ApplyFilter() {
    auto* m = (ListBoxModelNav*)listBox->model;
    if (!m) {
        return;
    }
    TempStr sel = SelectedPathTemp();
    FilterNavEntries(m, filterWords);
    listBox->SetModel(m);
    if (m->ItemsCount() == 0) {
        listBox->SetCurrentSelection(-1);
    } else {
        int idx = len(filterWords) > 0 ? 0 : FindEntryIndex(this, m, sel);
        SelectAndEnsureVisible(listBox, idx);
    }
    listBox->Invalidate();
}

void NavFilesInFolderWnd::ClearFilter() {
    if (!filterEdit || len(filterWords) == 0) {
        return;
    }
    filterWords.Reset();
    filterEdit->SetText(Str{}); // EN_CHANGE re-applies the (now empty) filter
}

void NavFilesInFolderWnd::OnDirLabelClick(VirtMouseEvent*) {
    BeginEditPath();
}

// clicking away restores the label, like Esc
void NavFilesInFolderWnd::OnDirEditKillFocus() {
    EndEditPath(NavPathEditEnd::Cancel);
}

// edit the path in place: the edit covers the label until Enter / Esc
void NavFilesInFolderWnd::BeginEditPath() {
    if (!dirEdit || editingPath) {
        return;
    }
    editingPath = true;
    dirEdit->SetText(currDir);
    dirEdit->SetBounds(PathEditRect());
    dirEdit->SetIsVisible(true);
    EditSetFocus(dirEdit);
    EditSelectAll(dirEdit);
}

// a little taller than the label so the border doesn't crowd the text
Rect NavFilesInFolderWnd::PathEditRect() {
    Rect r = dirLabel->VisibleRectInWindow();
    int pad = DpiScale(2);
    r.y -= pad;
    r.dy += 2 * pad;
    return r;
}

// Commit: a directory is navigated to, a file's directory with that file
// selected; anything else leaves the current dir (the label never changed)
void NavFilesInFolderWnd::EndEditPath(NavPathEditEnd how) {
    if (!editingPath) {
        return;
    }
    editingPath = false; // before hiding: that fires kill-focus
    TempStr path = dirEdit->GetTextTemp();
    dirEdit->SetIsVisible(false);
    // hiding the child doesn't repaint the label it covered
    HwndInvalidateRect(hwnd, PathEditRect(), true);
    SetFocusTo(listBox);
    if (how == NavPathEditEnd::Cancel) {
        return;
    }
    // Explorer's "Copy as path" wraps the path in quotes
    str::TrimWSInPlace(path, str::TrimOpt::Both);
    if (len(path) >= 2 && path.s[0] == '"' && path.s[len(path) - 1] == '"') {
        path = Str(path.s + 1, len(path) - 2);
    }
    if (dir::Exists(path)) {
        Navigate(path);
    } else if (file::Exists(path)) {
        Navigate(path::GetDirTemp(path), path);
    }
}

// typing while the list has focus goes into the search field
void NavFilesInFolderWnd::OnListChar(VirtCharEvent* ev) {
    if (ev->c < ' ' || IsCtrlPressed() || !filterEdit) {
        return;
    }
    EditSetFocus(filterEdit);
    SendMessageW(filterEdit->hwnd, WM_CHAR, (WPARAM)ev->c, 0);
    ev->didHandle = true;
}

// show dir and record it in the session history
void NavFilesInFolderWnd::Navigate(Str dir, Str selectPath) {
    NavHistoryPush(dir);
    SetDir(dir, selectPath);
}

void NavFilesInFolderWnd::SetDir(Str dir, Str selectPath, int selectIdx, NavListReset reset) {
    // the filter is per folder, like Explorer's search box
    if (!str::EqI(dir, currDir)) {
        ClearFilter();
    }
    str::ReplaceWithCopy(&currDir, dir);
    UpdateNavButtons();
    scanGen++;
    scanInFlight = true;
    str::ReplaceWithCopy(&pendingSelectPath, selectPath);
    pendingSelectIdx = selectIdx;

    auto* m = (ListBoxModelNav*)listBox->model;
    if (!m) {
        m = new ListBoxModelNav();
    }
    if (reset == NavListReset::Clear) {
        // show ".." immediately so the window is usable while the listing runs
        ClearNavModel(m);
        if (!IsHome()) {
            AppendNavParentEntry(m->all);
        }
        FilterNavEntries(m, filterWords);
        listBox->SetModel(m);
        UpdateDirLabel();
        if (m->ItemsCount() > 0) {
            SelectAndEnsureVisible(listBox, 0);
        }
        listBox->Invalidate();
    }

    auto* req = new NavDirScanReq;
    req->wnd = this;
    req->gen = scanGen;
    req->isRefresh = reset == NavListReset::Keep;
    req->dir = str::Dup(currDir);
    logf("NavDirScan: start %s gen=%d (UI thread=%d)\n", currDir, scanGen, (int)uitask::IsMainUIThread());
    auto fn = MkFunc0(NavDirScanThread, req);
    RunAsync(fn, StrL("NavDirScan"));
}

// full path of the selected entry, or empty. Owned copy: callers pass it back
// into SetDir(), which frees the entries the path would otherwise point into.
TempStr NavFilesInFolderWnd::SelectedPathTemp() {
    if (!listBox) {
        return {};
    }
    int idx = listBox->GetCurrentSelection();
    auto* m = (ListBoxModelNav*)listBox->model;
    if (!m || idx < 0 || idx >= m->ItemsCount()) {
        return {};
    }
    NavFileEntry& e = m->entries[idx];
    if (str::Eq(e.name, StrL(".."))) {
        return {};
    }
    return str::DupTemp(NavEntryPathTemp(this, e));
}

// re-read the directory, keeping the selection on the same file. The listing is
// a snapshot, so files renamed / added / removed after it was taken (by F2 in
// the main window, by another app, ...) would otherwise linger (issue #5878).
void NavFilesInFolderWnd::RefreshList() {
    // WM_ACTIVATE can arrive during CreateCustom(), before the list exists
    if (!listBox) {
        return;
    }
    TempStr sel = SelectedPathTemp();
    int selIdx = -1;
    // listing still in flight: keep the file we meant to select
    if (len(sel) == 0 && scanInFlight && len(pendingSelectPath) > 0) {
        sel = str::DupTemp(pendingSelectPath);
    } else if (len(sel) == 0 && scanInFlight) {
        selIdx = pendingSelectIdx;
    }
    TempStr dir = str::DupTemp(currDir);
    SetDir(dir, sel, selIdx, NavListReset::Keep);
}

// from a root directory (C:\) Up goes to the home view
void NavFilesInFolderWnd::GoUp() {
    if (IsHome()) {
        return;
    }
    if (!NavDirHasParent(currDir)) {
        GoHome();
        return;
    }
    // select the directory we're coming from
    TempStr cameFrom = str::DupTemp(currDir);
    Navigate(path::GetDirTemp(currDir), cameFrom);
}

void NavFilesInFolderWnd::GoBack() {
    if (gNavHistIdx <= 0) {
        return;
    }
    TempStr cameFrom = str::DupTemp(currDir);
    gNavHistIdx--;
    Str dir = gNavHistory[gNavHistIdx];
    SetDir(dir, SelectIfChildOf(cameFrom, dir));
}

void NavFilesInFolderWnd::GoForward() {
    if (gNavHistIdx + 1 >= len(gNavHistory)) {
        return;
    }
    TempStr cameFrom = str::DupTemp(currDir);
    gNavHistIdx++;
    Str dir = gNavHistory[gNavHistIdx];
    SetDir(dir, SelectIfChildOf(cameFrom, dir));
}

void NavFilesInFolderWnd::GoHome() {
    if (IsHome()) {
        return;
    }
    Navigate(Str{});
}

// inNewTab: Ctrl+Enter / Ctrl+double-click. Switches to the tab already showing
// the file, or opens it in a new tab, instead of replacing the current document.
void NavFilesInFolderWnd::ExecuteCurrentSelection(bool inNewTab) {
    int idx = listBox->GetCurrentSelection();
    auto* m = (ListBoxModelNav*)listBox->model;
    if (!m || idx < 0 || idx >= m->ItemsCount()) {
        return;
    }
    NavFileEntry& e = m->entries[idx];
    if (str::Eq(e.name, StrL(".."))) {
        GoUp();
        return;
    }
    TempStr path = NavEntryPathTemp(this, e);
    if (e.isDir) {
        Navigate(path);
        return;
    }

    MainWindow* mainWin = win;
    if (!IsMainWindowValidAndNotClosing(mainWin)) {
        ScheduleDeleteNavFilesWnd();
        return;
    }

    if (inNewTab) {
        WindowTab* existing = FindTabByFilePath(path);
        if (existing) {
            SelectTabInWindow(existing);
            if (existing->win) {
                SetForegroundWindow(existing->win->hwndFrame);
            }
            return;
        }
        DismissNextFileScrollHint(mainWin);
        LoadArgs args(path, mainWin);
        args.skipHistory = skipHistory;
        // no forceReuse: opens in a new tab, leaving the current document alone
        StartLoadDocument(&args);
        return;
    }

    WindowTab* tab = mainWin->CurrentTab();
    if (tab && !MaybeSaveAnnotations(tab)) {
        return;
    }
    DismissNextFileScrollHint(mainWin);
    LoadArgs args(path, mainWin);
    args.skipHistory = skipHistory;
    // replace the document in the current tab; keep this window open
    args.forceReuse = true;
    StartLoadDocument(&args);
    // Hand keyboard control back to the document, the way Ctrl + Enter does
    // when it switches to a tab that already has the file (issue #5903).
    // Loading is async and LoadModelIntoTab focuses the frame when it lands,
    // but that only helps if the frame is the foreground window by then - this
    // window is still on top otherwise, and arrow keys keep driving the list.
    // The window stays open so browsing can continue; it just isn't focused.
    SetForegroundWindow(mainWin->hwndFrame);
    HwndSetFocus(mainWin->hwndFrame);
}

// Del on a file moves it to the recycle bin (issue #5877), without a
// confirmation prompt -- the recycle bin is the undo. Directories are left
// alone: recursively deleting a folder from a file picker is too easy to
// trigger by accident, and "Show in Folder" + Explorer covers it.
void NavFilesInFolderWnd::DeleteCurrentSelection() {
    if (!CanAccessDisk() || gPluginMode) {
        return;
    }
    int idx = listBox->GetCurrentSelection();
    auto* m = (ListBoxModelNav*)listBox->model;
    if (!m || idx < 0 || idx >= m->ItemsCount()) {
        return;
    }
    NavFileEntry& e = m->entries[idx];
    if (e.isDir || str::Eq(e.name, StrL(".."))) {
        return;
    }
    // own the path: deleting re-fills the model, which frees the entry
    TempStr path = str::DupTemp(NavEntryPathTemp(this, e));
    if (!file::Exists(path)) {
        return;
    }

    // no confirmation prompt: the file goes to the recycle bin, so it's undoable

    // a document open in a tab keeps the file mapped, so the delete would fail;
    // close that tab first, like CmdDeleteFile does for the current document
    WindowTab* tab = FindTabByFilePath(path);
    if (tab) {
        if (!MaybeSaveAnnotations(tab)) {
            return;
        }
        CloseTab(tab, false);
    }
    DeleteFileFromDiskAndHistory(path);

    if (file::Exists(path)) {
        MessageBoxWarning(hwnd, fmt(Tr("Couldn't delete %s").s, path));
    }
    // re-list; keep the selection where the deleted entry was
    if (IsHome()) {
        ResetQuickAccessCache();
    }
    TempStr dir = str::DupTemp(currDir);
    SetDir(dir, Str{}, idx);
    SetFocusTo(listBox);
}

void NavFilesInFolderWnd::OnListDoubleClick() {
    ExecuteCurrentSelection(IsCtrlPressed());
}

void NavFilesInFolderWnd::OnKeyDown(KeyEvent* ev) {
    if (hwnd && ev->hwnd != hwnd && !IsChild(hwnd, ev->hwnd)) {
        return;
    }
    if (dirEdit && ev->hwnd == dirEdit->hwnd) {
        if (ev->vkey == VK_RETURN) {
            EndEditPath(NavPathEditEnd::Commit);
            ev->didHandle = true;
        } else if (ev->vkey == VK_ESCAPE) {
            EndEditPath(NavPathEditEnd::Cancel);
            ev->didHandle = true;
        }
        return;
    }
    if (ev->vkey == VK_RETURN) {
        ExecuteCurrentSelection(ev->isCtrl);
        ev->didHandle = true;
        return;
    }
    if (ev->vkey == 'F' && ev->isCtrl && !ev->isAlt && filterEdit) {
        EditSetFocus(filterEdit);
        EditSelectAll(filterEdit);
        ev->didHandle = true;
        return;
    }
    bool editFocused = filterEdit && ev->hwnd == filterEdit->hwnd;
    if (editFocused && !ev->isAlt) {
        // Up / Down / PgUp / PgDn move the list selection while typing;
        // Esc clears the filter; the edit keeps its other keys (Backspace, Del)
        switch (ev->vkey) {
            case VK_UP:
            case VK_DOWN:
            case VK_PRIOR:
            case VK_NEXT: {
                VirtKeyEvent kev;
                kev.target = listBox;
                kev.vkey = ev->vkey;
                kev.isCtrl = ev->isCtrl;
                kev.isShift = ev->isShift;
                listBox->OnKeyDown(&kev);
                ev->didHandle = true;
                return;
            }
            case VK_ESCAPE:
                if (len(filterWords) > 0) {
                    ClearFilter();
                    ev->didHandle = true;
                }
                return;
            case VK_F5:
                break;
            default:
                return;
        }
    }
    // Alt + Up / Left / Right go up / back / forward, like Explorer. They
    // arrive as WM_SYSKEYDOWN; swallowing them also avoids the system-menu beep.
    // Backspace goes up, like the classic Explorer / file dialogs
    if ((ev->vkey == VK_UP && ev->isAlt) || ev->vkey == VK_BACK) {
        GoUp();
        ev->didHandle = true;
        return;
    }
    if (ev->vkey == VK_LEFT && ev->isAlt) {
        GoBack();
        ev->didHandle = true;
        return;
    }
    if (ev->vkey == VK_RIGHT && ev->isAlt) {
        GoForward();
        ev->didHandle = true;
        return;
    }
    if (ev->vkey == VK_DELETE) {
        DeleteCurrentSelection();
        ev->didHandle = true;
        return;
    }
    if (ev->vkey == VK_F5) {
        if (IsHome()) {
            ResetQuickAccessCache();
        }
        RefreshList();
        ev->didHandle = true;
    }
}

static void NavButtonClicked(NavFilesInFolderWnd* w, VirtMouseEvent* ev) {
    auto* btn = (VirtIconButton*)ev->target;
    switch (btn->id - 1) {
        case NavBtnBack:
            w->GoBack();
            break;
        case NavBtnForward:
            w->GoForward();
            break;
        case NavBtnUp:
            w->GoUp();
            break;
        case NavBtnHome:
            w->GoHome();
            break;
    }
    w->SetFocusTo(w->listBox);
}

void NavFilesInFolderWnd::CreateNavButtons(Color fg, Color bg) {
    static const char* icons[NavBtnCount] = {gIconNavigateBack, gIconNavigateForward, gIconArrowUp, gIconHome};
    Str tips[NavBtnCount] = {fmt("%s (Alt + Left)", Tr("Back")), fmt("%s (Alt + Right)", Tr("Forward")),
                             fmt("%s (Alt + Up, Backspace)", Tr("Up")), Tr("Home")};
    int isz = RoundUp(DpiScale(16), 4);
    int pad = DpiScale(4);
    Color dis = ThemeWindowTextDisabledColor();
    for (int i = 0; i < NavBtnCount; i++) {
        auto* b = new VirtIconButton();
        b->id = i + 1; // 0 is "no id"
        b->padding = Insets{pad, pad, pad, pad};
        b->SetTooltip(tips[i]);
        b->pixmap = GetCachedPixmapForSvg(Str(icons[i]), isz, isz, fg, bg);
        b->pixmapDisabled = GetCachedPixmapForSvg(Str(icons[i]), isz, isz, dis, bg);
        b->onClick = MkFunc1(NavButtonClicked, this);
        navBtns[i] = b;
    }
}

// after activate: refresh dir listing and put focus on the list (Alt-Tab)
void NavFilesInFolderWnd::OnActivate(WindowBase::ActivateEvent* ev) {
    if (ev->state != WA_INACTIVE) {
        // first show already started a listing; don't kick off a second one
        if (!scanInFlight) {
            RefreshList();
        }
        ScheduleFocusNavListBox();
    }
}

// top-level received focus (e.g. Alt-Tab); steer it to the list
void NavFilesInFolderWnd::OnFocus(WindowBase::FocusEvent*) {
    // the list is a virtual control: this window holds the win32 focus on
    // its behalf, so only the focus inside the tree moves
    if (vroot && listBox) {
        vroot->SetFocus(listBox);
    }
}

void NavFilesInFolderWnd::DrawListBoxItem(VirtListBox::DrawItemEvent* ev) {
    VirtListBox* lb = ev->listBox;
    auto* m = (ListBoxModelNav*)lb->model;
    if (ev->itemIndex < 0 || ev->itemIndex >= m->ItemsCount()) {
        return;
    }

    Gfx* gfx = ev->gfx;
    HWND hwndList = lb->GetHwnd();
    Rect rc = ev->itemRect;
    NavFileEntry& e = m->entries[ev->itemIndex];

    Color colBg = lb->GetColor(kColListBg);
    Color colText = lb->GetColor(kColListText);
    if (IsSpecialColor(colBg)) {
        colBg = GetSysColor(COLOR_WINDOW);
    }
    if (IsSpecialColor(colText)) {
        colText = GetSysColor(COLOR_WINDOWTEXT);
    }

    if (ev->selected) {
        colBg = AccentColor(colBg, 30);
    }

    gfx->FillRect(rc, colBg);

    // drawing text into a mirrored surface would mirror the glyphs; we lay the
    // row out right-to-left ourselves instead
    bool isRtl = HwndIsRtl(hwndList);
    bool prevMirrored = isRtl ? gfx->SetMirrored(false) : false;

    int padX = DpiScale(4);
    rc.x += padX;
    rc.dx -= 2 * padX;

    // human readable file size on the right (files only; include 0-byte files)
    Rect rcText = rc;
    TempStr sizeStr;
    int rightDx = 0;
    if (!e.isDir) {
        sizeStr = str::FormatSizeShortTemp(e.size);
        rightDx = gfx->MeasureText(sizeStr, lb->font).dx;
        int gap = DpiScale(8);
        if (isRtl) {
            rcText.x += rightDx + gap;
            rcText.dx -= rightDx + gap;
        } else {
            rcText.dx -= rightDx + gap;
        }
    }

    {
        // directories in bold, without the trailing "\"; filter matches highlighted
        u32 drawFmt = gfxTextEllipsis | gfxTextVCenter;
        drawFmt |= isRtl ? (gfxTextRight | gfxTextRtl) : gfxTextLeft;
        PlatformFont* font = e.isDir ? GetBoldPlatformFont(lb->font) : lb->font;
        DrawMaybeHighlightedText(gfx, rcText, NavEntryBaseName(e), filterWords, highlighted, colBg, isRtl, false,
                                 drawFmt, font, colText);
    }

    if (sizeStr) {
        Rect rcRight = rc;
        u32 drawFmt = gfxTextVCenter;
        if (isRtl) {
            rcRight.dx = rightDx;
            drawFmt |= gfxTextLeft | gfxTextRtl;
        } else {
            rcRight.x = rc.x + rc.dx - rightDx;
            rcRight.dx = rightDx;
            drawFmt |= gfxTextRight;
        }
        gfx->DrawText(sizeStr, rcRight, drawFmt, lb->font, AccentColor(colText, 80));
    }

    if (isRtl) {
        gfx->SetMirrored(prevMirrored);
    }
}

// non-client (frame) size for an outer width/height of the given client size
static Size NavFrameChrome(HWND hwnd) {
    DWORD style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
    DWORD exStyle = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    RECT r{0, 0, 0, 0};
    AdjustWindowRectEx(&r, style, FALSE, exStyle);
    return {r.right - r.left, r.bottom - r.top};
}

// free horizontal space on the work area to the left / right of the main frame
static void NavFreeSpaceBeside(HWND hwndMain, int* freeLeftOut, int* freeRightOut) {
    Rect main = HwndWindowRect(hwndMain);
    Rect work = GetWorkAreaRect(main, hwndMain);
    *freeLeftOut = main.x - work.x;
    *freeRightOut = (work.x + work.dx) - (main.x + main.dx);
    *freeLeftOut = std::max(*freeLeftOut, 0);
    *freeRightOut = std::max(*freeRightOut, 0);
}

// pick docked client size when there is enough free space beside the main window.
// returns true and fills clientDx/clientDy; false means fall back to centered placement.
static bool NavDockedClientSize(HWND hwnd, HWND hwndMain, int* clientDxOut, int* clientDyOut, bool* placeLeftOut) {
    int freeLeft = 0;
    int freeRight = 0;
    NavFreeSpaceBeside(hwndMain, &freeLeft, &freeRight);

    int minFree = DpiScale(kNavDockMinFreeDx);
    int free = 0;
    bool placeLeft = false;
    // NOLINTNEXTLINE(bugprone-branch-clone): left is preferred first, then falls back after right
    if (freeLeft > freeRight && freeLeft > minFree) {
        free = freeLeft;
        placeLeft = true;
    } else if (freeRight > minFree) {
        free = freeRight;
        placeLeft = false;
    } else if (freeLeft > minFree) {
        free = freeLeft;
        placeLeft = true;
    } else {
        return false;
    }

    // outer width fits the free strip but is capped at kNavDockMaxWidthDx
    int maxOuterDx = DpiScale(kNavDockMaxWidthDx);
    int outerDx = free < maxOuterDx ? free : maxOuterDx;

    Rect main = HwndWindowRect(hwndMain);
    int outerDy = main.dy;
    Size chrome = NavFrameChrome(hwnd);
    int clientDx = outerDx - chrome.dx;
    int clientDy = outerDy - chrome.dy;
    int minClientDx = DpiScale(kNavMinClientDx);
    int minClientDy = DpiScale(kNavMinClientDy);
    clientDx = std::max(clientDx, minClientDx);
    clientDy = std::max(clientDy, minClientDy);
    *clientDxOut = clientDx;
    *clientDyOut = clientDy;
    *placeLeftOut = placeLeft;
    return true;
}

// place hwnd next to the main frame (docked) or centered over it (fallback)
static void PositionNavFilesWnd(HWND hwnd, HWND hwndMain, bool docked, bool placeLeft) {
    Rect main = HwndWindowRect(hwndMain);
    Rect r = HwndWindowRect(hwnd);
    int x;
    int y;
    if (docked) {
        y = main.y;
        if (placeLeft) {
            x = main.x - r.dx;
        } else {
            x = main.x + main.dx;
        }
    } else {
        x = main.x + (main.dx / 2) - (r.dx / 2);
        y = main.y + DpiScale(kNavFallbackYOffset);
    }
    Rect r2 = ShiftRectToWorkArea({x, y, r.dx, r.dy}, hwndMain, true);
    SetWindowPos(hwnd, nullptr, r2.x, r2.y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
}

// Start directory when no document is open (home page). The file-open dialog
// deliberately doesn't set an initial directory, letting the shell reopen the
// folder of the last file opened through it; the closest equivalent we can
// compute is the newest still-existing entry in our own file history.
static TempStr NavStartDirNoDocTemp() {
    for (int i = 0;; i++) {
        FileState* fs = FileHistoryGet(i);
        if (!fs) {
            break;
        }
        TempStr dir = path::GetDirTemp(fs->filePath);
        if (len(dir) > 0 && dir::Exists(dir)) {
            return dir;
        }
    }
    TempStr docs = GetSpecialFolderTemp(CSIDL_PERSONAL);
    if (len(docs) > 0 && dir::Exists(docs)) {
        return docs;
    }
    return GetSelfExeDirTemp();
}

// filePath: the file to browse to and select; empty for the current tab's
// folder or, on the home page, the newest history entry's
bool NavFilesInFolderWnd::Create(MainWindow* mainWin, Str filePath) {
    win = mainWin;
    {
        CreateCustomArgs args;
        args.visible = false;
        // regular resizable window (not a popup that auto-dismisses)
        args.style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;
        args.title = Tr("Navigate Files in Folder");
        args.font = GetFont();
        args.icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(GetAppIconID()));
        args.isRtl = IsUIRtl();
        CreateCustom(args);
    }
    if (!hwnd) {
        return false;
    }
    // top-level (no owner) so Alt-Tab switches between this and the main window
    DarkModeApplyToTitleBar(hwnd);

    auto colBg = ThemeWindowControlBackgroundColor();
    auto colTxt = ThemeWindowTextColor();
    SetColors(colTxt, colBg);
    CreateNavButtons(colTxt, colBg);

    auto* vbox = new VBox();
    vbox->alignMain = MainAxisAlign::MainStart;
    vbox->alignCross = CrossAxisAlign::Stretch;

    {
        // top row: Back / Forward / Up / Home buttons, then the current dir
        auto* row = new HBox();
        row->alignCross = CrossAxisAlign::CrossCenter;
        row->rtl = IsUIRtl();
        for (VirtIconButton* b : navBtns) {
            row->AddChild(b);
        }
        auto* c = NewVirtText({
            .font = font,
            .isRtl = IsUIRtl(),
            .ellipsis = true,
        });
        dirLabel = c;
        c->SetFlag(vwfNoHitTest, false); // plain text ignores the mouse
        c->cursor = CursorId::IBeam;
        c->SetTooltip(Tr("Click to edit the path"));
        c->onClick = MkMethod1<NavFilesInFolderWnd, VirtMouseEvent*, &NavFilesInFolderWnd::OnDirLabelClick>(this);
        row->AddChild(new Padding(c, Insets{0, 4, 0, 4}), 1);
        vbox->AddChild(new Padding(row, Insets{0, 0, 4, 0}));
    }

    {
        // path editor: not in the layout, placed over dirLabel while editing
        Edit::CreateArgs args;
        args.parent = hwnd;
        args.withBorder = true;
        args.font = font;
        args.isRtl = IsUIRtl();
        dirEdit = new Edit();
        dirEdit->SetColors(colTxt, colBg);
        dirEdit->Create(args);
        dirEdit->SetIsVisible(false);
        dirEdit->onKillFocus = MkMethod0<NavFilesInFolderWnd, &NavFilesInFolderWnd::OnDirEditKillFocus>(this);
    }

    {
        // second row: filters the list below as you type
        Edit::CreateArgs args;
        args.parent = hwnd;
        args.withBorder = true;
        args.cueText = Tr("Search");
        args.font = font;
        args.isRtl = IsUIRtl();
        filterEdit = new Edit();
        filterEdit->SetColors(colTxt, colBg);
        filterEdit->Create(args);
        filterEdit->onTextChanged = MkMethod0<NavFilesInFolderWnd, &NavFilesInFolderWnd::OnFilterChanged>(this);
        vbox->AddChild(new Padding(filterEdit, Insets{0, 0, 4, 0}));
    }

    {
        auto* c = new VirtListBox();
        c->dpi = GetDpi();
        c->font = font;
        c->padding = DpiScaledInsets(4, 0);
        c->onDoubleClick = MkMethod0<NavFilesInFolderWnd, &NavFilesInFolderWnd::OnListDoubleClick>(this);
        c->onChar = MkMethod1<NavFilesInFolderWnd, VirtCharEvent*, &NavFilesInFolderWnd::OnListChar>(this);
        c->onDrawItem =
            MkMethod1<NavFilesInFolderWnd, VirtListBox::DrawItemEvent*, &NavFilesInFolderWnd::DrawListBoxItem>(this);
        listBox = c;
        vbox->AddChild(c, 1);
    }

    {
        // one wrapping line of key-cap hints, like the command palette help
        // row; translators keep the key names in English
        TempStr hints = fmt("(Kbd/%s) %s (Kbd/%s) %s (Kbd/%s) %s", Tr("Enter"), Tr("open in current tab"),
                            Tr("Ctrl + Enter"), Tr("open in new tab"), Tr("Del"), Tr("delete file"));
        // the hints are secondary information, so they get a smaller font
        PlatformFont* helpFont = GetUserGuiFont({}, std::max(GetAppFontSize() - 2, 8));
        auto* k = new VirtRichText();
        ParseTipInto(k, hints);
        k->font = helpFont;
        k->SetColor(kColRichText, colTxt);
        k->SetColor(kColRichLink, colTxt);
        k->SetColor(kColRichBg, colBg);
        auto* center = new Align(k);
        center->HAlign = AlignCenter;
        vbox->AddChild(new Padding(center, Insets{DpiScale(4), 0, 0, 0}));
    }

    auto* padding = new Padding(vbox, DpiScaledInsets(4, 8));
    layout = padding;

    TempStr dir = len(filePath) > 0 ? path::GetDirTemp(filePath) : Str{};
    if (len(dir) == 0 || !dir::Exists(dir)) {
        dir = NavStartDirNoDocTemp();
    }
    Navigate(dir, filePath);
    // remember selection: layout below changes listbox size, so LB_SETCURSEL
    // during SetDir may not leave the item visible in the final viewport
    int selIdx = listBox->GetCurrentSelection();

    int dx = 0;
    int dy = 0;
    bool placeLeft = false;
    bool docked = NavDockedClientSize(hwnd, mainWin->hwndFrame, &dx, &dy, &placeLeft);
    if (!docked) {
        auto rc = HwndClientRect(mainWin->hwndFrame);
        dy = rc.dy - DpiScale(kNavFallbackMainDyMargin);
        dy = std::max(dy, DpiScale(kNavFallbackMinDy));
        dx = limitValue(rc.dx - DpiScale(kNavFallbackMainDxMargin), DpiScale(kNavFallbackMinDx),
                        DpiScale(kNavFallbackMaxDx));
    }
    LayoutAndSizeToContent(layout, dx, dy, hwnd);
    // pick up the virtual controls so we paint them and they get their input
    DoLayout();
    PositionNavFilesWnd(hwnd, mainWin->hwndFrame, docked, placeLeft);

    if (selIdx >= 0) {
        SelectAndEnsureVisible(listBox, selIdx);
    }

    SetIsVisible(true);
    SetFocusTo(listBox);
    return true;
}

void ShowNavFilesInFolder(MainWindow* win, Str selectPath, bool skipHistory) {
    // Prefer an explicit path (e.g. home-page thumbnail); else the current tab.
    Str filePath = selectPath;
    if (len(filePath) == 0) {
        WindowTab* tab = win->CurrentTab();
        if (tab && !tab->IsAboutTab()) {
            filePath = tab->filePath;
        }
    }

    if (gNavFilesWnd) {
        if (gNavFilesWnd->hwnd && IsWindow(gNavFilesWnd->hwnd)) {
            // re-sync to the target folder (and re-read: the file may have been
            // renamed since, #5878)
            if (len(filePath) > 0) {
                gNavFilesWnd->Navigate(path::GetDirTemp(filePath), filePath);
            } else {
                // on the home page with no selection keep whatever dir is open
                gNavFilesWnd->RefreshList();
            }
            ShowWindow(gNavFilesWnd->hwnd, SW_SHOW);
            SetForegroundWindow(gNavFilesWnd->hwnd);
            gNavFilesWnd->skipHistory = skipHistory;
            if (gNavFilesWnd->listBox) {
                gNavFilesWnd->SetFocusTo(gNavFilesWnd->listBox);
            } else {
                HwndSetFocus(gNavFilesWnd->hwnd);
            }
            return;
        }
        ScheduleDeleteNavFilesWnd();
    }
    auto* wnd = new NavFilesInFolderWnd();
    wnd->skipHistory = skipHistory;
    wnd->closeOnEsc = true;
    wnd->onClose = MkFunc0Void(ScheduleDeleteNavFilesWnd);
    wnd->onDestroy = MkFunc0Void(ScheduleDeleteNavFilesWnd);
    wnd->onActivate = MkMethod1<NavFilesInFolderWnd, WindowBase::ActivateEvent*, &NavFilesInFolderWnd::OnActivate>(wnd);
    wnd->onFocus = MkMethod1<NavFilesInFolderWnd, WindowBase::FocusEvent*, &NavFilesInFolderWnd::OnFocus>(wnd);
    wnd->onKeyDown = MkMethod1<NavFilesInFolderWnd, KeyEvent*, &NavFilesInFolderWnd::OnKeyDown>(wnd);
    wnd->SetFont(GetAppFont());
    // set before Create so Esc during Create can dismiss
    gNavFilesWnd = wnd;
    gHwndToActivateOnNavClose = win->hwndFrame;
    bool ok = wnd->Create(win, filePath);
    if (!ok) {
        gNavFilesWnd = nullptr;
        gHwndToActivateOnNavClose = nullptr;
        delete wnd;
    }
}

// State and actions used by the -dbg-control regression test.
TempStr NavFilesInFolderStateTemp(Str action, int idx, int* exitCodeOut) {
    if (exitCodeOut) {
        *exitCodeOut = 2;
    }
    NavFilesInFolderWnd* wnd = gNavFilesWnd;
    if (!wnd || !wnd->listBox || !wnd->listBox->model) {
        return str::DupTemp(StrL("NOTREADY no-window"));
    }
    if (str::Eq(action, StrL("select"))) {
        wnd->listBox->SetCurrentSelection(idx);
    } else if (str::Eq(action, StrL("delete-refresh"))) {
        wnd->DeleteCurrentSelection();
        wnd->RefreshList();
    } else if (str::Eq(action, StrL("up"))) {
        wnd->GoUp();
    } else if (str::Eq(action, StrL("back"))) {
        wnd->GoBack();
    } else if (str::Eq(action, StrL("forward"))) {
        wnd->GoForward();
    } else if (str::Eq(action, StrL("home"))) {
        wnd->GoHome();
    } else if (str::Eq(action, StrL("execute"))) {
        wnd->ExecuteCurrentSelection();
    } else if (str::Eq(action, StrL("refresh"))) {
        wnd->RefreshList();
    } else if (str::Eq(action, StrL("path-label-rect"))) {
        // where a test must click to start editing the path (client coords)
        Rect r = wnd->dirLabel->VisibleRectInWindow();
        if (exitCodeOut) {
            *exitCodeOut = 0;
        }
        return fmt("OK %d %d %d %d", r.x, r.y, r.dx, r.dy);
    } else if (str::Eq(action, StrL("list-screen-rect"))) {
        // where a test must touch to pan the list (screen coords)
        Rect r = wnd->listBox->VisibleRectInWindow();
        POINT pt{r.x, r.y};
        ClientToScreen(wnd->hwnd, &pt);
        if (exitCodeOut) {
            *exitCodeOut = 0;
        }
        return fmt("OK %d %d %d %d", (int)pt.x, (int)pt.y, r.dx, r.dy);
    } else if (str::Eq(action, StrL("edit-path")) || str::TrimPrefix(action, StrL("path-"))) {
        // path editing: edit-path, path-text:<text>, path-commit, path-cancel,
        // path-state (report only)
        if (str::Eq(action, StrL("edit-path"))) {
            wnd->BeginEditPath();
        } else if (str::Eq(action, StrL("commit"))) {
            wnd->EndEditPath(NavPathEditEnd::Commit);
        } else if (str::Eq(action, StrL("cancel"))) {
            wnd->EndEditPath(NavPathEditEnd::Cancel);
        } else if (str::TrimPrefix(action, StrL("text:"))) {
            wnd->dirEdit->SetText(action);
        }
        if (exitCodeOut) {
            *exitCodeOut = 0;
        }
        TempStr text = wnd->dirEdit->GetTextTemp();
        return fmt("OK editing=%d text=\"%s\"", (int)wnd->editingPath, text);
    } else if (str::TrimPrefix(action, StrL("filter:"))) {
        wnd->filterEdit->SetText(action);
        wnd->OnFilterChanged();
    } else if (str::Eq(action, StrL("close"))) {
        wnd->Close();
        if (exitCodeOut) {
            *exitCodeOut = 0;
        }
        return str::DupTemp(StrL("OK closed"));
    }

    auto* m = (ListBoxModelNav*)wnd->listBox->model;
    int sel = wnd->listBox->GetCurrentSelection();
    Str name;
    if (sel >= 0 && sel < m->ItemsCount()) {
        name = m->entries[sel].name;
    }
    if (exitCodeOut) {
        *exitCodeOut = 0;
    }
    int canBack = wnd->navBtns[NavBtnBack]->IsEnabled();
    int canFwd = wnd->navBtns[NavBtnForward]->IsEnabled();
    return fmt("OK scan=%d sel=%d items=%d back=%d fwd=%d dir=\"%s\" name=\"%s\" scrollY=%d", (int)wnd->scanInFlight,
               sel, m->ItemsCount(), canBack, canFwd, wnd->currDir, name, wnd->listBox->scrollY);
}

// ============================================================================
// Docked Right-Side Folder Files Playlist Sidebar (FolderFilesPanel)
// ============================================================================

constexpr int kFolderSidebarDefaultWidthDip = 260;
constexpr int kFolderSidebarMinWidthDip = 180;
constexpr int kFolderSidebarMaxWidthDip = 600;

static bool gFolderSidebarSettingsLoaded = false;
static bool gFolderSidebarEnabled = true;
static int gFolderSidebarWidthDip = kFolderSidebarDefaultWidthDip;
static u64 gNextFolderPanelToken = 0;

static int ParsePositiveIntSlice(Str s, int defVal) {
    str::TrimWSInPlace(s, str::TrimOpt::Both);
    if (len(s) <= 0) {
        return defVal;
    }
    int val = 0;
    bool anyDigit = false;
    for (int i = 0; i < len(s); i++) {
        char c = s.s[i];
        if (c < '0' || c > '9') {
            break;
        }
        anyDigit = true;
        val = (val * 10) + (c - '0');
        if (val > 10000) {
            break;
        }
    }
    return anyDigit ? val : defVal;
}

static TempStr GetFolderSidebarConfigPathTemp() {
    if (!CanAccessDisk() || gPluginMode) {
        return {};
    }
    TempStr dir = GetSumatraDataDirTemp();
    if (len(dir) == 0) {
        return {};
    }
    return path::JoinTemp(dir, StrL("SumatraPDF-folder-sidebar.ini"));
}

static void EnsureFolderSidebarSettingsLoaded() {
    if (gFolderSidebarSettingsLoaded) {
        return;
    }
    gFolderSidebarSettingsLoaded = true;
    TempStr cfgPath = GetFolderSidebarConfigPathTemp();
    if (len(cfgPath) == 0 || !file::Exists(cfgPath)) {
        return;
    }
    Str data = file::ReadFile(cfgPath);
    if (len(data) == 0) {
        str::Free(data);
        return;
    }
    Str rest = data;
    Str line;
    while (str::NextLine(rest, line, rest)) {
        str::TrimWSInPlace(line, str::TrimOpt::Both);
        if (str::TrimPrefix(line, StrL("enabled="))) {
            int v = ParsePositiveIntSlice(line, 1);
            gFolderSidebarEnabled = (v != 0);
        } else if (str::TrimPrefix(line, StrL("width="))) {
            int w = ParsePositiveIntSlice(line, kFolderSidebarDefaultWidthDip);
            gFolderSidebarWidthDip = limitValue(w, kFolderSidebarMinWidthDip, kFolderSidebarMaxWidthDip);
        }
    }
    str::Free(data);
}

static void SaveFolderSidebarSettings() {
    if (gDontSaveSettings) {
        return;
    }
    TempStr cfgPath = GetFolderSidebarConfigPathTemp();
    if (len(cfgPath) == 0) {
        return;
    }
    TempStr content = fmt("enabled=%d\nwidth=%d\n", gFolderSidebarEnabled ? 1 : 0, gFolderSidebarWidthDip);
    file::WriteFile(cfgPath, content);
}

bool IsFolderFilesSidebarEnabled() {
    EnsureFolderSidebarSettingsLoaded();
    return gFolderSidebarEnabled;
}

enum FolderPanelBtn {
    FolderBtnBack = 0,
    FolderBtnForward = 1,
    FolderBtnUp = 2,
    FolderBtnRefresh = 3,
    FolderBtnCount = 4
};

struct ListBoxModelFolder : ListBoxModelNav {
    Vec<int> fileOrd;
    int totalFiles = 0;
};

static void RebuildFolderModelEntries(ListBoxModelFolder* m, const StrVec& words) {
    FilterNavEntries(m, words);
    VecReset(m->fileOrd);
    int ord = 0;
    for (int i = 0; i < len(m->entries); i++) {
        if (m->entries[i].isDir) {
            VecAppend(m->fileOrd, 0);
        } else {
            ord++;
            VecAppend(m->fileOrd, ord);
        }
    }
    m->totalFiles = ord;
}

struct FolderFilesPanel {
    MainWindow* ownerWin = nullptr;
    u64 token = 0;
    UINT_PTR boxSubclassId = 0;
    UINT_PTR filterSubclassId = 0;

    VirtRoot* root = nullptr;
    ILayout* layout = nullptr;
    HBox* headerBox = nullptr;
    VirtText* headerLabel = nullptr;
    VirtCloseButton* closeBtn = nullptr;
    VirtIconButton* navBtns[FolderBtnCount]{};
    VirtText* dirLabel = nullptr;
    Edit* filterEdit = nullptr;
    VirtListBox* listBox = nullptr;

    StrVec filterWords;
    Vec<u8> highlighted;
    Str currDir;
    Str activeFilePath;
    Str pendingSelectPath;
    StrVec history;
    int histIdx = -1;
    int scanGen = 0;
    bool scanInFlight = false;
    int pressedItemIdx = -1;

    ~FolderFilesPanel() {
        scanGen++;
        str::Free(currDir);
        str::Free(activeFilePath);
        str::Free(pendingSelectPath);
    }
};

static TempStr FolderEntryPathTemp(FolderFilesPanel* panel, const NavFileEntry& e) {
    if (str::Eq(e.name, StrL(".."))) {
        return path::GetDirTemp(panel->currDir);
    }
    if (e.path) {
        return e.path;
    }
    Str name = e.name;
    if (e.isDir && len(name) > 0) {
        name = Str(name.s, len(name) - 1);
    }
    return path::JoinTemp(panel->currDir, name);
}

static int FindFolderEntryIndex(FolderFilesPanel* panel, ListBoxModelFolder* m, Str selectPath) {
    if (!m || len(selectPath) == 0) {
        return -1;
    }
    for (int i = 0; i < len(m->entries); i++) {
        TempStr p = FolderEntryPathTemp(panel, m->entries[i]);
        if (str::EqI(p, selectPath) || path::IsSame(p, selectPath)) {
            return i;
        }
    }
    TempStr base = path::GetBaseNameTemp(selectPath);
    for (int i = 0; i < len(m->entries); i++) {
        if (str::EqI(NavEntryBaseName(m->entries[i]), base)) {
            return i;
        }
    }
    return -1;
}

static void FolderPanelPushHistory(FolderFilesPanel* panel, Str dir) {
    if (len(dir) == 0) {
        return;
    }
    if (panel->histIdx >= 0 && panel->histIdx < len(panel->history) &&
        path::IsSame(panel->history[panel->histIdx], dir)) {
        return;
    }
    while (len(panel->history) > panel->histIdx + 1) {
        panel->history.RemoveAt(len(panel->history) - 1);
    }
    if (len(panel->history) >= kNavHistoryMax) {
        panel->history.RemoveAt(0);
    }
    panel->history.Append(dir);
    panel->histIdx = len(panel->history) - 1;
}

static void UpdateFolderPanelHeaderAndButtons(FolderFilesPanel* panel) {
    if (!panel) {
        return;
    }
    if (panel->headerLabel) {
        auto* m = panel->listBox ? (ListBoxModelFolder*)panel->listBox->model : nullptr;
        int fileCount = m ? m->totalFiles : 0;
        if (fileCount > 0) {
            panel->headerLabel->SetText(fmt("%s (%d)", Tr("Files in Folder"), fileCount));
        } else {
            panel->headerLabel->SetText(Tr("Files in Folder"));
        }
        panel->headerLabel->Invalidate();
    }
    if (panel->dirLabel) {
        if (len(panel->currDir) == 0) {
            panel->dirLabel->SetText(Str{});
            panel->dirLabel->SetTooltip(Str{});
        } else {
            TempStr leaf = path::GetBaseNameTemp(panel->currDir);
            Str shown = len(leaf) > 0 ? (Str)leaf : panel->currDir;
            panel->dirLabel->SetText(shown);
            panel->dirLabel->SetTooltip(panel->currDir);
        }
        panel->dirLabel->Invalidate();
    }
    bool canBack = panel->histIdx > 0;
    bool canFwd = (panel->histIdx >= 0) && (panel->histIdx + 1 < len(panel->history));
    bool canUp = len(panel->currDir) > 0 && NavDirHasParent(panel->currDir);
    bool canRefresh = len(panel->currDir) > 0;
    bool states[FolderBtnCount] = {canBack, canFwd, canUp, canRefresh};
    for (int i = 0; i < FolderBtnCount; i++) {
        if (panel->navBtns[i]) {
            panel->navBtns[i]->SetIsEnabled(states[i]);
        }
    }
}

struct FolderScanReq {
    HWND hwndFrame = nullptr;
    u64 token = 0;
    int gen = 0;
    bool isRefresh = false;
    Str dir;
    ~FolderScanReq() { str::Free(dir); }
};

struct FolderScanResult {
    HWND hwndFrame = nullptr;
    u64 token = 0;
    int gen = 0;
    bool isRefresh = false;
    Str dir;
    Vec<NavFileEntry> entries;
    ~FolderScanResult() {
        str::Free(dir);
        for (NavFileEntry& e : entries) {
            FreeNavEntry(e);
        }
    }
};

static void FinishFolderDirScan(FolderScanResult* r) {
    AutoDelete del(r);
    MainWindow* mw = FindMainWindowByHwnd(r->hwndFrame);
    if (!mw || !IsMainWindowValidAndNotClosing(mw)) {
        return;
    }
    FolderFilesPanel* panel = mw->folderFilesPanel;
    if (!panel || panel->token != r->token || panel->scanGen != r->gen || !panel->listBox) {
        return;
    }
    panel->scanInFlight = false;
    auto* m = (ListBoxModelFolder*)panel->listBox->model;
    if (!m) {
        m = new ListBoxModelFolder();
    }
    if (r->isRefresh && SameNavEntries(m->all, r->entries)) {
        UpdateFolderPanelHeaderAndButtons(panel);
        panel->listBox->Invalidate();
        return;
    }
    int prevScrollY = panel->listBox->scrollY;
    StealNavEntries(m->all, r->entries);
    RebuildFolderModelEntries(m, panel->filterWords);
    panel->listBox->SetModel(m);
    UpdateFolderPanelHeaderAndButtons(panel);

    int selIdx = -1;
    if (len(panel->pendingSelectPath) > 0) {
        selIdx = FindFolderEntryIndex(panel, m, panel->pendingSelectPath);
    }
    if (selIdx < 0 && len(panel->activeFilePath) > 0) {
        selIdx = FindFolderEntryIndex(panel, m, panel->activeFilePath);
    }
    if (m->ItemsCount() > 0) {
        if (r->isRefresh) {
            if (selIdx >= 0) {
                panel->listBox->SetCurrentSelection(std::min(selIdx, m->ItemsCount() - 1));
            }
            panel->listBox->ScrollTo(prevScrollY);
        } else if (selIdx >= 0) {
            SelectAndEnsureVisible(panel->listBox, selIdx);
        } else {
            panel->listBox->SetCurrentSelection(0);
        }
    }
    panel->listBox->Invalidate();
}

static void FolderDirScanThread(FolderScanReq* req) {
    AutoDelete delReq(req);
    auto* r = new FolderScanResult();
    r->hwndFrame = req->hwndFrame;
    r->token = req->token;
    r->gen = req->gen;
    r->isRefresh = req->isRefresh;
    r->dir = str::Dup(req->dir);
    if (len(req->dir) > 0) {
        CollectNavEntriesForDir(req->dir, r->entries);
        // Remove synthetic ".." row when at drive root (Up button already disabled)
        if (!NavDirHasParent(req->dir) && len(r->entries) > 0 && str::Eq(r->entries[0].name, StrL(".."))) {
            FreeNavEntry(r->entries[0]);
            VecRemoveAt(r->entries, 0);
        }
    }
    auto fn = MkFunc0(FinishFolderDirScan, r);
    uitask::Post(fn, "FinishFolderDirScan");
}

static void FolderPanelSetDir(FolderFilesPanel* panel, Str dir, Str selectPath, NavListReset reset) {
    if (!panel || !panel->listBox || !panel->ownerWin) {
        return;
    }
    if (!path::IsSame(dir, panel->currDir) && panel->filterEdit && len(panel->filterWords) > 0) {
        panel->filterWords.Reset();
        panel->filterEdit->SetText(Str{});
    }
    str::ReplaceWithCopy(&panel->currDir, dir);
    str::ReplaceWithCopy(&panel->pendingSelectPath, selectPath);
    panel->scanGen++;
    panel->scanInFlight = true;

    auto* m = (ListBoxModelFolder*)panel->listBox->model;
    if (!m) {
        m = new ListBoxModelFolder();
    }
    if (reset == NavListReset::Clear) {
        ClearNavModel(m);
        if (len(dir) > 0 && NavDirHasParent(dir)) {
            AppendNavParentEntry(m->all);
        }
        RebuildFolderModelEntries(m, panel->filterWords);
        panel->listBox->SetModel(m);
        panel->listBox->Invalidate();
    }
    UpdateFolderPanelHeaderAndButtons(panel);

    auto* req = new FolderScanReq();
    req->hwndFrame = panel->ownerWin->hwndFrame;
    req->token = panel->token;
    req->gen = panel->scanGen;
    req->isRefresh = (reset == NavListReset::Keep);
    req->dir = str::Dup(panel->currDir);
    RunAsync(MkFunc0(FolderDirScanThread, req), StrL("FolderDirScan"));
}

static void FolderPanelNavigate(FolderFilesPanel* panel, Str dir, Str selectPath = {},
                                NavListReset reset = NavListReset::Clear) {
    if (len(dir) == 0 || !dir::Exists(dir)) {
        return;
    }
    FolderPanelPushHistory(panel, dir);
    FolderPanelSetDir(panel, dir, selectPath, reset);
}

static void FolderPanelGoUp(FolderFilesPanel* panel) {
    if (!panel || len(panel->currDir) == 0 || !NavDirHasParent(panel->currDir)) {
        return;
    }
    TempStr cameFrom = str::DupTemp(panel->currDir);
    TempStr parentDir = path::GetDirTemp(panel->currDir);
    if (len(parentDir) > 0 && dir::Exists(parentDir)) {
        FolderPanelNavigate(panel, parentDir, cameFrom);
    }
}

static void FolderPanelGoBack(FolderFilesPanel* panel) {
    if (!panel || panel->histIdx <= 0) {
        return;
    }
    TempStr cameFrom = str::DupTemp(panel->currDir);
    panel->histIdx--;
    Str targetDir = panel->history[panel->histIdx];
    FolderPanelSetDir(panel, targetDir, SelectIfChildOf(cameFrom, targetDir), NavListReset::Clear);
}

static void FolderPanelGoForward(FolderFilesPanel* panel) {
    if (!panel || panel->histIdx + 1 >= len(panel->history)) {
        return;
    }
    TempStr cameFrom = str::DupTemp(panel->currDir);
    panel->histIdx++;
    Str targetDir = panel->history[panel->histIdx];
    FolderPanelSetDir(panel, targetDir, SelectIfChildOf(cameFrom, targetDir), NavListReset::Clear);
}

static void FolderPanelRefresh(FolderFilesPanel* panel) {
    if (!panel || len(panel->currDir) == 0) {
        return;
    }
    TempStr dir = str::DupTemp(panel->currDir);
    TempStr sel = str::DupTemp(panel->activeFilePath);
    FolderPanelSetDir(panel, dir, sel, NavListReset::Keep);
}

static void FolderPanelActivateIndex(FolderFilesPanel* panel, int idx, bool inNewTab) {
    if (!panel || !panel->listBox || !panel->ownerWin) {
        return;
    }
    auto* m = (ListBoxModelFolder*)panel->listBox->model;
    if (!m || idx < 0 || idx >= m->ItemsCount()) {
        return;
    }
    NavFileEntry& e = m->entries[idx];
    if (str::Eq(e.name, StrL(".."))) {
        FolderPanelGoUp(panel);
        return;
    }
    TempStr targetPath = str::DupTemp(FolderEntryPathTemp(panel, e));
    if (e.isDir) {
        FolderPanelNavigate(panel, targetPath);
        return;
    }

    MainWindow* mw = panel->ownerWin;
    if (!IsMainWindowValidAndNotClosing(mw)) {
        return;
    }

    if (inNewTab) {
        WindowTab* existing = FindTabInWindow(targetPath, mw);
        if (!existing) {
            existing = FindTabByFilePath(targetPath);
        }
        if (existing) {
            SelectTabInWindow(existing);
            if (existing->win) {
                HwndSetFocus(existing->win->hwndFrame);
            }
            return;
        }
        DismissNextFileScrollHint(mw);
        LoadArgs args(targetPath, mw);
        StartLoadDocument(&args);
        HwndSetFocus(mw->hwndFrame);
        return;
    }

    WindowTab* curTab = mw->CurrentTab();
    if (curTab && !curTab->IsAboutTab() && len(curTab->filePath) > 0 && path::IsSame(curTab->filePath, targetPath)) {
        HwndSetFocus(mw->hwndFrame);
        return;
    }
    if (curTab && !MaybeSaveAnnotations(curTab)) {
        return;
    }
    DismissNextFileScrollHint(mw);
    str::ReplaceWithCopy(&panel->activeFilePath, targetPath);
    panel->listBox->SetCurrentSelection(idx);
    panel->listBox->Invalidate();

    LoadArgs args(targetPath, mw);
    args.forceReuse = true;
    StartLoadDocument(&args);
    HwndSetFocus(mw->hwndFrame);
}

static void OnFolderFilterChanged(FolderFilesPanel* panel) {
    if (!panel || !panel->filterEdit || !panel->listBox) {
        return;
    }
    TempStr s = panel->filterEdit->GetTextTemp();
    panel->filterWords.Reset();
    SplitFilterToWords(s, panel->filterWords);

    auto* m = (ListBoxModelFolder*)panel->listBox->model;
    if (!m) {
        return;
    }
    RebuildFolderModelEntries(m, panel->filterWords);
    panel->listBox->SetModel(m);
    UpdateFolderPanelHeaderAndButtons(panel);

    if (m->ItemsCount() == 0) {
        panel->listBox->SetCurrentSelection(-1);
    } else if (len(panel->filterWords) > 0) {
        SelectAndEnsureVisible(panel->listBox, 0);
    } else {
        int idx = FindFolderEntryIndex(panel, m, panel->activeFilePath);
        SelectAndEnsureVisible(panel->listBox, idx >= 0 ? idx : 0);
    }
    panel->listBox->Invalidate();
}

static void OnFolderListMouseDown(FolderFilesPanel* panel, VirtMouseEvent* ev) {
    if (!panel || !panel->listBox) {
        return;
    }
    VirtListBox* lb = panel->listBox;
    if (ev->button == 0) {
        panel->pressedItemIdx = lb->ItemFromPoint(ev->pt);
        lb->OnMouseDown(ev);
        return;
    }
    if (ev->button == 2) {
        int idx = lb->ItemFromPoint(ev->pt);
        panel->pressedItemIdx = idx;
        if (idx >= 0) {
            lb->SetCurrentSelection(idx);
        }
        ev->didHandle = true;
        return;
    }
    panel->pressedItemIdx = -1;
}

static void OnFolderListMouseUp(FolderFilesPanel* panel, VirtMouseEvent* ev) {
    if (!panel || !panel->listBox) {
        return;
    }
    VirtListBox* lb = panel->listBox;
    int downIdx = panel->pressedItemIdx;
    panel->pressedItemIdx = -1;

    if (ev->button == 0) {
        lb->OnMouseUp(ev);
        int upIdx = lb->ItemFromPoint(ev->pt);
        if (downIdx >= 0 && upIdx == downIdx) {
            FolderPanelActivateIndex(panel, upIdx, ev->isCtrl);
        }
        return;
    }
    if (ev->button == 2) {
        int upIdx = lb->ItemFromPoint(ev->pt);
        if (downIdx >= 0 && upIdx == downIdx) {
            FolderPanelActivateIndex(panel, upIdx, true);
        }
        ev->didHandle = true;
    }
}

static void OnFolderListDoubleClick(FolderFilesPanel* panel, VirtMouseEvent* ev) {
    if (panel) {
        panel->pressedItemIdx = -1;
    }
    ev->didHandle = true;
}

static void OnFolderListKeyDown(FolderFilesPanel* panel, VirtKeyEvent* ev) {
    if (!panel || !panel->listBox) {
        return;
    }
    if (ev->vkey == VK_RETURN) {
        int sel = panel->listBox->GetCurrentSelection();
        if (sel >= 0) {
            FolderPanelActivateIndex(panel, sel, ev->isCtrl);
        }
        ev->didHandle = true;
        return;
    }
    if ((ev->vkey == VK_UP && ev->isAlt) || ev->vkey == VK_BACK) {
        FolderPanelGoUp(panel);
        ev->didHandle = true;
        return;
    }
    if (ev->vkey == VK_LEFT && ev->isAlt) {
        FolderPanelGoBack(panel);
        ev->didHandle = true;
        return;
    }
    if (ev->vkey == VK_RIGHT && ev->isAlt) {
        FolderPanelGoForward(panel);
        ev->didHandle = true;
        return;
    }
    if (ev->vkey == VK_F5) {
        FolderPanelRefresh(panel);
        ev->didHandle = true;
        return;
    }
    if (ev->vkey == VK_ESCAPE) {
        if (panel->filterEdit && len(panel->filterWords) > 0) {
            panel->filterWords.Reset();
            panel->filterEdit->SetText(Str{});
        } else if (panel->ownerWin) {
            HwndSetFocus(panel->ownerWin->hwndFrame);
        }
        ev->didHandle = true;
        return;
    }
    panel->listBox->OnKeyDown(ev);
}

static void OnFolderListChar(FolderFilesPanel* panel, VirtCharEvent* ev) {
    if (!panel || !panel->filterEdit || ev->c < ' ' || IsCtrlPressed()) {
        return;
    }
    EditSetFocus(panel->filterEdit);
    SendMessageW(panel->filterEdit->hwnd, WM_CHAR, (WPARAM)ev->c, 0);
    ev->didHandle = true;
}

static void OnFolderListTooltip(FolderFilesPanel* panel, VirtTooltipEvent* ev) {
    if (!panel || !panel->listBox) {
        return;
    }
    int idx = panel->listBox->ItemFromPoint(ev->ptLocal);
    auto* m = (ListBoxModelFolder*)panel->listBox->model;
    if (!m || idx < 0 || idx >= m->ItemsCount()) {
        return;
    }
    const NavFileEntry& e = m->entries[idx];
    if (str::Eq(e.name, StrL(".."))) {
        ev->tip = str::DupTemp(Tr("Up"));
        return;
    }
    if (e.isDir) {
        ev->tip = str::DupTemp(FolderEntryPathTemp(panel, e));
        return;
    }
    ev->tip = fmt("%s (%s)", NavEntryBaseName(e), str::FormatSizeShortTemp(e.size));
}

static void DrawFolderListBoxItem(FolderFilesPanel* panel, VirtListBox::DrawItemEvent* ev) {
    VirtListBox* lb = ev->listBox;
    auto* m = (ListBoxModelFolder*)lb->model;
    if (!panel || !m || ev->itemIndex < 0 || ev->itemIndex >= m->ItemsCount()) {
        return;
    }

    Gfx* gfx = ev->gfx;
    HWND hwndHost = lb->GetHwnd();
    int dpi = hwndHost ? DpiGetForHwnd(hwndHost) : lb->dpi;
    Rect rc = ev->itemRect;
    const NavFileEntry& e = m->entries[ev->itemIndex];
    TempStr entryPath = FolderEntryPathTemp(panel, e);

    bool isActiveDoc = !e.isDir && len(panel->activeFilePath) > 0 &&
                       (str::EqI(entryPath, panel->activeFilePath) || path::IsSame(entryPath, panel->activeFilePath));

    Color colBg = lb->GetColor(kColListBg);
    Color colText = lb->GetColor(kColListText);
    if (IsSpecialColor(colBg)) {
        colBg = GetSysColor(COLOR_WINDOW);
    }
    if (IsSpecialColor(colText)) {
        colText = GetSysColor(COLOR_WINDOWTEXT);
    }

    Color rowBg = colBg;
    if (isActiveDoc) {
        rowBg = AccentColor(colBg, 40);
    } else if (ev->selected) {
        rowBg = AccentColor(colBg, 22);
    }
    gfx->FillRect(rc, rowBg);

    bool isRtl = hwndHost ? HwndIsRtl(hwndHost) : false;
    bool prevMirrored = isRtl ? gfx->SetMirrored(false) : false;

    if (isActiveDoc) {
        int barW = std::max(DpiScaleByDpi(dpi, 3), 2);
        int barPadY = DpiScaleByDpi(dpi, 2);
        int barX = isRtl ? (rc.Right() - barW) : rc.x;
        Rect barRc{barX, rc.y + barPadY, barW, std::max(rc.dy - (2 * barPadY), 2)};
        gfx->FillRect(barRc, AccentColor(colText, 35));
    }

    int padX = DpiScaleByDpi(dpi, 6);
    rc.x += padX;
    rc.dx = std::max(rc.dx - (2 * padX), 0);

    // Left index column ("01", "02", ...) for files
    int fileOrd = (ev->itemIndex < len(m->fileOrd)) ? m->fileOrd[ev->itemIndex] : 0;
    TempStr ordStr{};
    int leftReserve = 0;
    if (!e.isDir && fileOrd > 0) {
        ordStr = (fileOrd < 100) ? fmt("%02d", fileOrd) : fmt("%d", fileOrd);
        int minOrdDx = gfx->MeasureText(StrL("00"), lb->font).dx;
        int ordDx = std::max(minOrdDx, gfx->MeasureText(ordStr, lb->font).dx);
        int ordGap = DpiScaleByDpi(dpi, 6);
        leftReserve = ordDx + ordGap;

        Rect rcOrd = rc;
        u32 ordFmt = gfxTextVCenter;
        if (isRtl) {
            rcOrd.x = rc.Right() - ordDx;
            rcOrd.dx = ordDx;
            ordFmt |= gfxTextRight;
        } else {
            rcOrd.dx = ordDx;
            ordFmt |= gfxTextLeft;
        }
        gfx->DrawText(ordStr, rcOrd, ordFmt, lb->font, AccentColor(colText, isActiveDoc ? 35 : 95));
    }

    // Right badge ("P.<page>" if read/active, else short file size)
    TempStr rightBadge{};
    int rightReserve = 0;
    bool hasPageBadge = false;
    if (!e.isDir) {
        int pageNo = 0;
        if (isActiveDoc && panel->ownerWin && panel->ownerWin->currPageNo > 0) {
            pageNo = panel->ownerWin->currPageNo;
        } else if (FileHistoryStates()) {
            FileState* fs = FileHistoryFindByPath(entryPath);
            if (fs && len(fs->pageNo) > 0) {
                StoredPagePos pos = ParseStoredPagePos(fs->pageNo);
                if (pos.pageNo > 0) {
                    pageNo = pos.pageNo;
                }
            }
        }
        if (pageNo > 0) {
            rightBadge = fmt("P.%d", pageNo);
            hasPageBadge = true;
        } else {
            rightBadge = str::FormatSizeShortTemp(e.size);
        }
        int badgeDx = gfx->MeasureText(rightBadge, lb->font).dx;
        int badgeGap = DpiScaleByDpi(dpi, 6);
        rightReserve = badgeDx + badgeGap;

        Rect rcRight = rc;
        u32 badgeFmt = gfxTextVCenter;
        if (isRtl) {
            rcRight.dx = badgeDx;
            badgeFmt |= gfxTextLeft | gfxTextRtl;
        } else {
            rcRight.x = rc.Right() - badgeDx;
            rcRight.dx = badgeDx;
            badgeFmt |= gfxTextRight;
        }
        Color badgeCol = AccentColor(colText, (isActiveDoc || hasPageBadge) ? 55 : 95);
        gfx->DrawText(rightBadge, rcRight, badgeFmt, lb->font, badgeCol);
    }

    Rect rcText = rc;
    if (isRtl) {
        rcText.x += rightReserve;
        rcText.dx = std::max(rcText.dx - leftReserve - rightReserve, 0);
    } else {
        rcText.x += leftReserve;
        rcText.dx = std::max(rcText.dx - leftReserve - rightReserve, 0);
    }

    if (rcText.dx > 0) {
        u32 drawFmt = gfxTextEllipsis | gfxTextVCenter;
        drawFmt |= isRtl ? (gfxTextRight | gfxTextRtl) : gfxTextLeft;
        PlatformFont* itemFont = (e.isDir || isActiveDoc) ? GetBoldPlatformFont(lb->font) : lb->font;
        Str displayTitle = e.isDir && !str::Eq(e.name, StrL("..")) ? e.name : NavEntryBaseName(e);
        DrawMaybeHighlightedText(gfx, rcText, displayTitle, panel->filterWords, panel->highlighted, rowBg, isRtl, false,
                                 drawFmt, itemFont, colText);
    }

    if (isRtl) {
        gfx->SetMirrored(prevMirrored);
    }
}

static void OnFolderNavBtnClick(FolderFilesPanel* panel, VirtMouseEvent* ev) {
    if (!panel || !ev->target) {
        return;
    }
    auto* btn = (VirtIconButton*)ev->target;
    switch (btn->id - 1) {
        case FolderBtnBack:
            FolderPanelGoBack(panel);
            break;
        case FolderBtnForward:
            FolderPanelGoForward(panel);
            break;
        case FolderBtnUp:
            FolderPanelGoUp(panel);
            break;
        case FolderBtnRefresh:
            FolderPanelRefresh(panel);
            break;
    }
}

static void OnFolderCloseClick(MainWindow* mw, VirtMouseEvent* /*ev*/) {
    OnFolderFilesToggle(mw, 0);
}

static LRESULT CALLBACK WndProcFolderFilterEdit(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR /*subclassId*/,
                                                DWORD_PTR data) {
    auto* panel = (FolderFilesPanel*)data;
    if (panel && panel->listBox && msg == WM_KEYDOWN) {
        if (wp == VK_UP || wp == VK_DOWN || wp == VK_PRIOR || wp == VK_NEXT) {
            VirtKeyEvent kev;
            kev.target = panel->listBox;
            kev.vkey = (int)wp;
            kev.isCtrl = IsCtrlPressed();
            kev.isShift = IsShiftPressed();
            panel->listBox->OnKeyDown(&kev);
            return 0;
        }
        if (wp == VK_RETURN) {
            int sel = panel->listBox->GetCurrentSelection();
            if (sel >= 0) {
                FolderPanelActivateIndex(panel, sel, IsCtrlPressed());
            }
            return 0;
        }
        if (wp == VK_ESCAPE) {
            TempStr txt = panel->filterEdit ? panel->filterEdit->GetTextTemp() : TempStr{};
            if (len(txt) > 0) {
                panel->filterEdit->SetText(Str{});
            } else if (panel->ownerWin) {
                HwndSetFocus(panel->ownerWin->hwndFrame);
            }
            return 0;
        }
        if (wp == VK_F5) {
            FolderPanelRefresh(panel);
            return 0;
        }
    }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == '\r' || wp == '\n')) {
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void LayoutFolderFilesBox(MainWindow* mw) {
    if (!mw || !mw->hwndFolderFilesBox || !mw->folderFilesPanel || !mw->folderFilesPanel->layout) {
        return;
    }
    Rect rc = HwndClientRect(mw->hwndFolderFilesBox);
    if (rc.dx <= 0 || rc.dy <= 0) {
        return;
    }
    FolderFilesPanel* panel = mw->folderFilesPanel;
    LayoutTreeToSize(mw->hwndFolderFilesBox, panel->layout, {rc.dx, rc.dy}, &panel->root);
}

static LRESULT CALLBACK WndProcFolderFilesBox(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR /*subclassId*/,
                                              DWORD_PTR data) {
    MainWindow* mw = (MainWindow*)data;
    if (!mw || !mw->folderFilesPanel) {
        return DefSubclassProc(hwnd, msg, wp, lp);
    }
    FolderFilesPanel* panel = mw->folderFilesPanel;

    LRESULT res = TryReflectMessages(hwnd, msg, wp, lp);
    if (res) {
        return res;
    }

    if (VirtHostOnMessage(hwnd, panel->root, msg, wp, lp, res, ThemeControlBackgroundColor())) {
        return res;
    }

    switch (msg) {
        case WM_ERASEBKGND: {
            HDC hdc = (HDC)wp;
            HdcFillRect(hdc, HwndClientRect(hwnd), mw->brControlBgColor);
            return TRUE;
        }
        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wp;
            SetTextColor(hdc, ThemeWindowTextColor());
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)mw->brControlBgColor;
        }
        case WM_SIZE:
            LayoutFolderFilesBox(mw);
            break;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void OnFolderFilesSplitterMove(VirtSplitter::MoveEvent* ev) {
    MainWindow* mw = FindMainWindowByHwnd(ev->w->GetHwnd());
    if (!mw || !mw->hwndFolderFilesBox) {
        return;
    }
    Point pcur = HwndGetCursorPos(mw->hwndFrame);
    Rect rFrame = HwndClientRect(mw->hwndFrame);
    Rect boxRc = ChildPosWithinParent(mw->hwndFolderFilesBox);
    bool isRtlLayout = mw->frameLayout && mw->frameLayout->rtl;
    int dx = isRtlLayout ? (pcur.x - boxRc.x) : (boxRc.Right() - pcur.x);

    int dpi = DpiGetForHwnd(mw->hwndFrame);
    int minDx = DpiScaleByDpi(dpi, kFolderSidebarMinWidthDip);
    int maxDx = std::max(minDx, rFrame.dx / 2);
    if (dx < minDx || dx > maxDx) {
        ev->resizeAllowed = false;
        return;
    }
    if (ev->queryOnly) {
        return;
    }
    mw->folderFilesDx = dx;
    if (dpi > 0) {
        gFolderSidebarWidthDip =
            limitValue(MulDiv(dx, 96, dpi), kFolderSidebarMinWidthDip, kFolderSidebarMaxWidthDip);
    }
    u32 flags = kUiRelayout | kUiNoToolbars;
    if (!ev->finishedDragging) {
        flags |= kUiPanelsDrag;
    } else {
        SaveFolderSidebarSettings();
    }
    ScheduleUiUpdate(mw, flags);
}

static void UpdateFolderNavButtonIcons(FolderFilesPanel* panel, int dpi) {
    if (!panel) {
        return;
    }
    static const char* icons[FolderBtnCount] = {gIconNavigateBack, gIconNavigateForward, gIconArrowUp,
                                                gIconRotateRight};
    Str tips[FolderBtnCount] = {Tr("Back"), Tr("Forward"), Tr("Up"), Tr("Refresh Folder")};
    int isz = RoundUp(DpiScaleByDpi(dpi, 15), 4);
    int pad = DpiScaleByDpi(dpi, 3);
    Color fg = ThemeWindowTextColor();
    Color bg = ThemeControlBackgroundColor();
    Color dis = ThemeWindowTextDisabledColor();
    for (int i = 0; i < FolderBtnCount; i++) {
        VirtIconButton* b = panel->navBtns[i];
        if (!b) {
            continue;
        }
        b->padding = Insets{pad, pad, pad, pad};
        b->SetTooltip(tips[i]);
        b->pixmap = GetCachedPixmapForSvg(Str(icons[i]), isz, isz, fg, bg);
        b->pixmapDisabled = GetCachedPixmapForSvg(Str(icons[i]), isz, isz, dis, bg);
        b->Invalidate();
    }
}

static void UpdateFolderListMetricsAndColors(FolderFilesPanel* panel, int dpi) {
    if (!panel || !panel->listBox) {
        return;
    }
    Color panelBg = ThemeControlBackgroundColor();
    Color listBg = ThemeWindowControlBackgroundColor();
    Color txtCol = ThemeWindowTextColor();

    if (panel->headerLabel) {
        panel->headerLabel->SetColor(kColTextFg, txtCol);
    }
    if (panel->dirLabel) {
        panel->dirLabel->SetColor(kColTextFg, AccentColor(txtCol, 65));
    }
    if (panel->filterEdit) {
        panel->filterEdit->SetColors(txtCol, listBg);
    }
    panel->listBox->dpi = dpi;
    panel->listBox->SetColor(kColListBg, listBg);
    panel->listBox->SetColor(kColListText, txtCol);
    Size textSz = PlatformFontMeasureText(panel->listBox->font, StrL("Ag"));
    panel->listBox->itemDy = std::max(textSz.dy + DpiScaleByDpi(dpi, 7), DpiScaleByDpi(dpi, 22));
    (void)panelBg;
}

static bool ShouldShowFolderFilesForWindow(MainWindow* mw) {
    if (!mw || !mw->hwndFolderFilesBox || !mw->folderFilesPanel || gPluginMode || !CanAccessDisk()) {
        return false;
    }
    if (mw->isQuickLook) {
        return false;
    }
    EnsureFolderSidebarSettingsLoaded();
    if (!gFolderSidebarEnabled) {
        return false;
    }
    WindowTab* tab = mw->CurrentTab();
    if (!tab || tab->IsAboutTab() || tab->IsFavoritesTab()) {
        return false;
    }
    if (len(tab->filePath) == 0) {
        return false;
    }
    TempStr dir = path::GetDirTemp(tab->filePath);
    if (len(dir) == 0 || !dir::Exists(dir)) {
        return false;
    }
    return true;
}

void CreateFolderFilesPanel(MainWindow* win) {
    if (!win || gPluginMode || !CanAccessDisk()) {
        return;
    }
    EnsureFolderSidebarSettingsLoaded();
    int dpi = DpiGetForHwnd(win->hwndFrame);
    win->folderFilesDx = DpiScaleByDpi(dpi, gFolderSidebarWidthDip);

    HMODULE hmod = GetModuleHandleW(nullptr);
    DWORD style = WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    win->hwndFolderFilesBox =
        CreateWindowExW(0, WC_STATIC, L"", style, 0, 0, win->folderFilesDx, 0, win->hwndFrame, nullptr, hmod, nullptr);
    if (!win->hwndFolderFilesBox) {
        return;
    }
    ShowWindow(win->hwndFolderFilesBox, SW_HIDE);

    if (win->folderFilesSplitter) {
        win->folderFilesSplitter->SetIsVisible(false);
        win->folderFilesSplitter->onMove = MkFunc1Void(OnFolderFilesSplitterMove);
    }

    auto* panel = new FolderFilesPanel();
    panel->ownerWin = win;
    panel->token = ++gNextFolderPanelToken;
    win->folderFilesPanel = panel;

    PlatformFont* appFont = GetAppFontForDpi(dpi);
    PlatformFont* treeFont = GetAppTreeFontForDpi(dpi);
    PlatformFont* labelFont = GetAppSidebarLabelFontForDpi(dpi);

    auto header = NewLabelWithClose(win->hwndFolderFilesBox, labelFont, MkFunc1(OnFolderCloseClick, win));
    panel->headerBox = header.box;
    panel->headerLabel = header.label;
    panel->closeBtn = header.closeBtn;
    panel->headerLabel->SetText(Tr("Files in Folder"));

    auto* navRow = new HBox();
    navRow->alignMain = MainAxisAlign::MainStart;
    navRow->alignCross = CrossAxisAlign::CrossCenter;
    navRow->rtl = IsUIRtl();
    for (int i = 0; i < FolderBtnCount; i++) {
        auto* b = new VirtIconButton();
        b->id = i + 1;
        b->onClick = MkFunc1(OnFolderNavBtnClick, panel);
        panel->navBtns[i] = b;
        navRow->AddChild(b);
    }
    panel->dirLabel = NewVirtText({
        .font = appFont,
        .isRtl = IsUIRtl(),
        .ellipsis = true,
    });
    navRow->AddChild(new Padding(panel->dirLabel, Insets{0, DpiScaleByDpi(dpi, 4), 0, DpiScaleByDpi(dpi, 2)}), 1);

    {
        Edit::CreateArgs eargs;
        eargs.parent = win->hwndFolderFilesBox;
        eargs.withBorder = true;
        eargs.cueText = Tr("Search");
        eargs.font = appFont;
        eargs.isRtl = IsUIRtl();
        panel->filterEdit = new Edit();
        panel->filterEdit->Create(eargs);
        panel->filterEdit->onTextChanged = MkFunc0(OnFolderFilterChanged, panel);
        panel->filterSubclassId = NextSubclassId();
        SetWindowSubclass(panel->filterEdit->hwnd, WndProcFolderFilterEdit, panel->filterSubclassId, (DWORD_PTR)panel);
    }

    {
        auto* lb = new VirtListBox();
        lb->dpi = dpi;
        lb->font = treeFont;
        lb->showFocusRing = false;
        lb->padding = DpiScaledInsets(2, 0);
        lb->onMouseDown = MkFunc1(OnFolderListMouseDown, panel);
        lb->onMouseUp = MkFunc1(OnFolderListMouseUp, panel);
        lb->VirtCtrl::onDoubleClick = MkFunc1(OnFolderListDoubleClick, panel);
        lb->onKeyDown = MkFunc1(OnFolderListKeyDown, panel);
        lb->onChar = MkFunc1(OnFolderListChar, panel);
        lb->onGetTooltip = MkFunc1(OnFolderListTooltip, panel);
        lb->onDrawItem = MkFunc1(DrawFolderListBoxItem, panel);
        panel->listBox = lb;
    }

    auto* vbox = new VBox();
    vbox->alignMain = MainAxisAlign::MainStart;
    vbox->alignCross = CrossAxisAlign::Stretch;
    vbox->AddChild(panel->headerBox);
    vbox->AddChild(new Padding(navRow, Insets{0, 0, DpiScaleByDpi(dpi, 2), 0}));
    vbox->AddChild(new Padding(panel->filterEdit, Insets{0, 0, DpiScaleByDpi(dpi, 3), 0}));
    vbox->AddChild(panel->listBox, 1);

    panel->layout = new Padding(vbox, DpiScaledInsets(4, 6));

    panel->boxSubclassId = NextSubclassId();
    SetWindowSubclass(win->hwndFolderFilesBox, WndProcFolderFilesBox, panel->boxSubclassId, (DWORD_PTR)win);

    UpdateFolderFilesTheme(win);
    UpdateFolderPanelHeaderAndButtons(panel);
}

void DestroyFolderFilesPanel(MainWindow* win) {
    if (!win) {
        return;
    }
    FolderFilesPanel* panel = win->folderFilesPanel;
    win->folderFilesPanel = nullptr;

    if (panel) {
        panel->scanGen++;
        if (panel->filterEdit && panel->filterEdit->hwnd && panel->filterSubclassId) {
            RemoveWindowSubclass(panel->filterEdit->hwnd, WndProcFolderFilterEdit, panel->filterSubclassId);
            panel->filterSubclassId = 0;
        }
        if (win->hwndFolderFilesBox && panel->boxSubclassId) {
            RemoveWindowSubclass(win->hwndFolderFilesBox, WndProcFolderFilesBox, panel->boxSubclassId);
            panel->boxSubclassId = 0;
        }
        delete panel->layout;
        panel->layout = nullptr;
        delete panel->root;
        panel->root = nullptr;
        delete panel;
    }

    if (win->folderFilesSplitter) {
        win->folderFilesSplitter->SetIsVisible(false);
        win->folderFilesSplitter->onMove = {};
    }

    if (win->hwndFolderFilesBox) {
        DestroyWindow(win->hwndFolderFilesBox);
        win->hwndFolderFilesBox = nullptr;
    }
}

void SyncFolderFilesToCurrentTab(MainWindow* win, bool forceRefresh) {
    if (!win || !win->folderFilesPanel) {
        return;
    }
    FolderFilesPanel* panel = win->folderFilesPanel;
    bool prevVis = win->uiState.folderFilesVisible;
    bool wantVis = ShouldShowFolderFilesForWindow(win);
    win->uiState.folderFilesVisible = wantVis;

    if (!wantVis) {
        str::ReplaceWithCopy(&panel->activeFilePath, Str{});
        if (prevVis != wantVis) {
            ScheduleUiUpdate(win);
        }
        return;
    }

    WindowTab* tab = win->CurrentTab();
    Str filePath = tab ? tab->filePath : Str{};
    TempStr dir = path::GetDirTemp(filePath);
    str::ReplaceWithCopy(&panel->activeFilePath, filePath);

    bool sameDir = len(panel->currDir) > 0 && path::IsSame(panel->currDir, dir);
    if (sameDir && !forceRefresh) {
        auto* m = (ListBoxModelFolder*)panel->listBox->model;
        if (m) {
            int idx = FindFolderEntryIndex(panel, m, filePath);
            if (idx >= 0) {
                panel->listBox->SetCurrentSelection(idx);
            }
        }
        panel->listBox->Invalidate();
    } else if (sameDir && forceRefresh) {
        FolderPanelSetDir(panel, dir, filePath, NavListReset::Keep);
    } else {
        FolderPanelNavigate(panel, dir, filePath, NavListReset::Clear);
    }

    if (prevVis != wantVis) {
        ScheduleUiUpdate(win);
    }
}

void OnFolderFilesToggle(MainWindow* win, int forceState) {
    EnsureFolderSidebarSettingsLoaded();
    bool newState = !gFolderSidebarEnabled;
    if (forceState == 0) {
        newState = false;
    } else if (forceState == 1) {
        newState = true;
    }
    gFolderSidebarEnabled = newState;
    SaveFolderSidebarSettings();

    for (MainWindow* mw : gWindows) {
        SyncFolderFilesToCurrentTab(mw, false);
        if (mw->menu) {
            UpdateAppMenu(mw, mw->menu);
        }
        ScheduleUiUpdate(mw);
    }
    if (win && !newState) {
        HwndSetFocus(win->hwndFrame);
    }
}

void UpdateFolderFilesTheme(MainWindow* win) {
    if (!win || !win->hwndFolderFilesBox || !win->folderFilesPanel) {
        return;
    }
    FolderFilesPanel* panel = win->folderFilesPanel;
    int dpi = DpiGetForHwnd(win->hwndFolderFilesBox);
    UpdateFolderListMetricsAndColors(panel, dpi);
    UpdateFolderNavButtonIcons(panel, dpi);
    DarkModeApplyToChildControls(win->hwndFolderFilesBox);
    RedrawWindow(win->hwndFolderFilesBox, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void UpdateFolderFilesDpi(MainWindow* win, int dpi) {
    if (!win || !win->hwndFolderFilesBox || !win->folderFilesPanel || dpi <= 0) {
        return;
    }
    FolderFilesPanel* panel = win->folderFilesPanel;
    win->folderFilesDx = DpiScaleByDpi(dpi, gFolderSidebarWidthDip);

    PlatformFont* appFont = GetAppFontForDpi(dpi);
    PlatformFont* treeFont = GetAppTreeFontForDpi(dpi);
    PlatformFont* labelFont = GetAppSidebarLabelFontForDpi(dpi);

    if (panel->headerLabel && panel->closeBtn) {
        ApplyLabelWithCloseDpi(panel->headerLabel, panel->closeBtn, dpi);
        panel->headerLabel->font = labelFont;
    }
    if (panel->dirLabel) {
        panel->dirLabel->font = appFont;
    }
    if (panel->filterEdit) {
        panel->filterEdit->SetFont(appFont);
    }
    if (panel->listBox) {
        panel->listBox->font = treeFont;
    }
    UpdateFolderListMetricsAndColors(panel, dpi);
    UpdateFolderNavButtonIcons(panel, dpi);
    RelayoutFolderFilesPanel(win);
    HwndInvalidate(win->hwndFolderFilesBox, true);
}

void RelayoutFolderFilesPanel(MainWindow* win) {
    if (!win || !win->hwndFolderFilesBox || !win->uiState.folderFilesVisible) {
        return;
    }
    LayoutFolderFilesBox(win);
    RedrawWindow(win->hwndFolderFilesBox, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);
    if (win->folderFilesSplitter) {
        win->folderFilesSplitter->Invalidate();
    }
}

void InvalidateFolderFilesPanel(MainWindow* win) {
    if (!win || !win->hwndFolderFilesBox || !win->uiState.folderFilesVisible || !win->folderFilesPanel) {
        return;
    }
    if (win->folderFilesPanel->listBox) {
        win->folderFilesPanel->listBox->Invalidate();
    }
}
