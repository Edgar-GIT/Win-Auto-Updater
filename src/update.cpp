#include "update.hpp"
#include "com_ptr.hpp"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <initguid.h>
#include <wuapi.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

namespace {

// IIDs for COM callback interfaces not declared in MinGW headers
// IDownloadProgressChangedCallback: C6E1C796-9746-4DD8-A78F-A5F4F8BDFBB3
// IDownloadCompletedCallback: 2C059B3A-2FB8-43C7-8E42-06720A8D56B7
// IInstallationProgressChangedCallback: 2B13BDD9-94D6-4253-8B46-96D1CA1A3013
// IInstallationCompletedCallback: E4E2F910-DC0A-4B81-9C92-4E996E8C2282

const GUID IID_DownloadProgress = 
    {0xC6E1C796, 0x9746, 0x4DD8, {0xA7, 0x8F, 0xA5, 0xF4, 0xF8, 0xBD, 0xFB, 0xB3}};
const GUID IID_DownloadComplete = 
    {0x2C059B3A, 0x2FB8, 0x43C7, {0x8E, 0x42, 0x06, 0x72, 0x0A, 0x8D, 0x56, 0xB7}};
const GUID IID_InstallProgress = 
    {0x2B13BDD9, 0x94D6, 0x4253, {0x8B, 0x46, 0x96, 0xD1, 0xCA, 0x1A, 0x30, 0x13}};
const GUID IID_InstallComplete = 
    {0xE4E2F910, 0xDC0A, 0x4B81, {0x9C, 0x92, 0x4E, 0x99, 0x6E, 0x8C, 0x22, 0x82}};

// Combined vtable: IUnknown(3) + one Invoke entry.
// All four callback interfaces share the same layout (3 IUnknown + 1 Invoke),
// so a single vtable works for all.

typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void*, REFIID, void**);
    ULONG (STDMETHODCALLTYPE *AddRef)(void*);
    ULONG (STDMETHODCALLTYPE *Release)(void*);
    HRESULT (STDMETHODCALLTYPE *Invoke)(void*, void*, void*);
} CallbackVtbl;

struct CallbackObj {
    CallbackVtbl* lpVtbl;
    LONG refCount;
};

HRESULT STDMETHODCALLTYPE callbackQI(void* self, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown ||
        IsEqualGUID(riid, IID_DownloadProgress) ||
        IsEqualGUID(riid, IID_DownloadComplete) ||
        IsEqualGUID(riid, IID_InstallProgress) ||
        IsEqualGUID(riid, IID_InstallComplete)) {
        *ppv = self;
        static_cast<CallbackObj*>(self)->lpVtbl->AddRef(self);
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE callbackAddRef(void* self) {
    return InterlockedIncrement(&static_cast<CallbackObj*>(self)->refCount);
}

ULONG STDMETHODCALLTYPE callbackRelease(void* self) {
    LONG r = InterlockedDecrement(&static_cast<CallbackObj*>(self)->refCount);
    if (r == 0) delete static_cast<CallbackObj*>(self);
    return r;
}

HRESULT STDMETHODCALLTYPE callbackInvoke(void*, void*, void*) {
    return S_OK;
}

CallbackVtbl g_callbackVtbl = {callbackQI, callbackAddRef, callbackRelease, callbackInvoke};

CallbackObj* makeCallback() {
    auto* obj = new CallbackObj{&g_callbackVtbl, 1};
    return obj;
}

constexpr const wchar_t* kSearchCriteria = L"IsInstalled=0 and IsHidden=0";

DEFINE_GUID(CLSID_UpdateCollection, 0x07f7438c, 0x7709, 0x4ca5, 0xb5, 0x18, 0x91, 0x27, 0x92, 0x88, 0x13, 0x4e);

long collectionCount(IUpdateCollection* collection) {
    if (!collection) {
        return 0;
    }
    LONG count = 0;
    if (FAILED(collection->get_Count(&count))) {
        return 0;
    }
    return count;
}

std::wstring toLower(std::wstring value) {
    for (wchar_t& c : value) {
        if (c >= L'A' && c <= L'Z') {
            c = static_cast<wchar_t>(c - L'A' + L'a');
        }
    }
    return value;
}

std::wstring updateTitle(IUpdate* update) {
    if (!update) {
        return L"Update";
    }
    BStr title;
    if (FAILED(update->get_Title(title.put())) || !title) {
        return L"Update";
    }
    return std::wstring(title.get());
}

std::wstring updateTitle(IUpdateCollection* collection, LONG index) {
    if (!collection || index < 0) {
        return L"Update";
    }
    ComPtr<IUpdate> update;
    if (FAILED(collection->get_Item(index, update.put())) || !update) {
        return L"Update";
    }
    return updateTitle(update.get());
}

std::wstring formatHresult(LONG hr) {
    wchar_t buffer[32]{};
    swprintf(buffer, 32, L"0x%08X", static_cast<unsigned long>(hr));
    return buffer;
}

std::wstring operationResultName(OperationResultCode code) {
    switch (code) {
        case orcNotStarted:
            return L"NotStarted";
        case orcInProgress:
            return L"InProgress";
        case orcSucceeded:
            return L"Succeeded";
        case orcSucceededWithErrors:
            return L"SucceededWithErrors";
        case orcFailed:
            return L"Failed";
        case orcAborted:
            return L"Aborted";
        default:
            return L"Unknown";
    }
}

bool acceptEula(IUpdate* update) {
    if (!update) {
        return false;
    }
    VARIANT_BOOL accepted = VARIANT_FALSE;
    if (SUCCEEDED(update->get_EulaAccepted(&accepted)) && accepted == VARIANT_TRUE) {
        return true;
    }
    return SUCCEEDED(update->AcceptEula());
}

bool isDefenderRelatedUpdate(std::wstring_view title) {
    const std::wstring lower = toLower(std::wstring(title));
    static const wchar_t* patterns[] = {
        L"definition update",
        L"defender",
        L"antimalware",
        L"anti-malware",
        L"security intelligence",
        L"malware protection",
        L"virus definition",
        L"platform update for microsoft defender",
        L"smartscreen",
    };
    for (const wchar_t* pattern : patterns) {
        if (lower.find(pattern) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

bool resultNeedsReboot(IInstallationResult* result) {
    if (!result) {
        return false;
    }
    VARIANT_BOOL required = VARIANT_FALSE;
    if (FAILED(result->get_RebootRequired(&required))) {
        return false;
    }
    return required == VARIANT_TRUE;
}

bool operationOk(OperationResultCode code) {
    return code == orcSucceeded || code == orcSucceededWithErrors;
}

bool ensureWindowsUpdateService() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
        return false;
    }

    SC_HANDLE service = OpenServiceW(manager, L"wuauserv", SERVICE_START | SERVICE_QUERY_STATUS);
    if (!service) {
        CloseServiceHandle(manager);
        return false;
    }

    SERVICE_STATUS status{};
    bool running = false;
    if (QueryServiceStatus(service, &status)) {
        running = status.dwCurrentState == SERVICE_RUNNING;
        if (!running && status.dwCurrentState == SERVICE_STOPPED) {
            running = StartServiceW(service, 0, nullptr) != FALSE;
            if (running) {
                for (int i = 0; i < 30; ++i) {
                    Sleep(1000);
                    if (QueryServiceStatus(service, &status) &&
                        status.dwCurrentState == SERVICE_RUNNING) {
                        running = true;
                        break;
                    }
                }
            }
        } else if (status.dwCurrentState == SERVICE_RUNNING) {
            running = true;
        }
    }

    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return running;
}

bool systemNeedsReboot() {
    HKEY key = nullptr;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing",
                      0, KEY_READ, &key) == ERROR_SUCCESS) {
        DWORD value = 0;
        DWORD size = sizeof(value);
        const LSTATUS status = RegQueryValueExW(key, L"RebootPending", nullptr, nullptr,
                                                reinterpret_cast<LPBYTE>(&value), &size);
        RegCloseKey(key);
        if (status == ERROR_SUCCESS) {
            return true;
        }
    }

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Control\\Session Manager",
                      0, KEY_READ, &key) == ERROR_SUCCESS) {
        wchar_t buffer[1]{};
        DWORD size = 0;
        const LSTATUS status = RegQueryValueExW(key, L"PendingFileRenameOperations", nullptr,
                                                nullptr, reinterpret_cast<LPBYTE>(buffer), &size);
        RegCloseKey(key);
        if (status == ERROR_SUCCESS && size > 0) {
            return true;
        }
    }

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Updates",
                      0, KEY_READ, &key) == ERROR_SUCCESS) {
        wchar_t buffer[1]{};
        DWORD size = 0;
        const LSTATUS status = RegQueryValueExW(key, L"UpdateExeVolatile", nullptr,
                                                nullptr, reinterpret_cast<LPBYTE>(buffer), &size);
        RegCloseKey(key);
        if (status == ERROR_SUCCESS && size > 0) {
            return true;
        }
    }

    return false;
}

}  // namespace

bool UpdateEngine::hasPendingRestart(IUpdateSession* session) const {
    log(L"Checking for pending restart via Windows Update Agent API...");

    {
        ComPtr<ISystemInformation> sysInfo;
        HRESULT hr = CoCreateInstance(CLSID_SystemInformation, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_ISystemInformation, reinterpret_cast<void**>(sysInfo.put()));
        if (SUCCEEDED(hr) && sysInfo) {
            VARIANT_BOOL reboot = VARIANT_FALSE;
            hr = sysInfo->get_RebootRequired(&reboot);
            log(L"  ISystemInformation::get_RebootRequired = " + std::to_wstring(reboot == VARIANT_TRUE));
            if (SUCCEEDED(hr) && reboot == VARIANT_TRUE) {
                log(L"  -> Pending restart detected (ISystemInformation).");
                return true;
            }
        } else {
            log(L"  ISystemInformation co-create failed: " + formatHresult(hr));
        }
    }

    {
        ComPtr<IUpdateInstaller> installer;
        HRESULT hr = session->CreateUpdateInstaller(installer.put());
        if (SUCCEEDED(hr) && installer) {
            VARIANT_BOOL rebootRequired = VARIANT_FALSE;
            hr = installer->get_RebootRequiredBeforeInstallation(&rebootRequired);
            log(L"  IUpdateInstaller::get_RebootRequiredBeforeInstallation = " +
                std::to_wstring(rebootRequired == VARIANT_TRUE));
            if (SUCCEEDED(hr) && rebootRequired == VARIANT_TRUE) {
                log(L"  -> Pending restart detected (RebootRequiredBeforeInstallation).");
                return true;
            }
        } else {
            log(L"  IUpdateInstaller create failed: " + formatHresult(hr));
        }
    }

    log(L"  WUAPI reports no pending restart. Checking system-level indicators...");
    if (systemNeedsReboot()) {
        log(L"  -> Pending restart detected via system-level indicators.");
        return true;
    }

    log(L"  No pending restart detected.");
    return false;
}

ComPtr<IUpdateCollection> UpdateEngine::makeSingleCollection(IUpdate* update) const {
    ComPtr<IUpdateCollection> collection;

    log(L"Creating UpdateCollection...");
    HRESULT hr = CoCreateInstance(CLSID_UpdateCollection, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IUpdateCollection, reinterpret_cast<void**>(collection.put()));
    log(L"CoCreateInstance HRESULT = " + formatHresult(hr));
    if (FAILED(hr)) {
        log(L"  FAILED: CoCreateInstance for IUpdateCollection.");
        return {};
    }
    if (!collection) {
        log(L"  FAILED: CoCreateInstance returned null collection.");
        return {};
    }
    if (!update) {
        log(L"  FAILED: Update pointer is null.");
        return {};
    }

    VARIANT_BOOL readOnly = VARIANT_FALSE;
    hr = collection->get_ReadOnly(&readOnly);
    log(L"Collection ReadOnly = " + std::to_wstring(readOnly == VARIANT_TRUE));

    log(L"Adding update...");
    LONG index = 0;
    hr = collection->Add(update, &index);
    log(L"Add HRESULT = " + formatHresult(hr) + L", index = " + std::to_wstring(index));
    if (FAILED(hr)) {
        log(L"  FAILED: Add update to collection.");
        return {};
    }

    return collection;
}

ComPtr<IUpdateCollection> UpdateEngine::makeCollectionFallback(IUpdateCollection* searchResults,
                                                               LONG updateIndex) const {
    log(L"--- Fallback: creating collection via Copy from search results ---");

    if (!searchResults) {
        log(L"  FAILED: search results collection is null.");
        return {};
    }

    log(L"Creating writable copy of search results collection...");
    ComPtr<IUpdateCollection> copy;
    HRESULT hr = searchResults->Copy(copy.put());
    log(L"Copy HRESULT = " + formatHresult(hr));
    if (FAILED(hr) || !copy) {
        log(L"  FAILED: Copy of search results.");
        return {};
    }

    VARIANT_BOOL readOnly = VARIANT_FALSE;
    hr = copy->get_ReadOnly(&readOnly);
    log(L"Copy ReadOnly = " + std::to_wstring(readOnly == VARIANT_TRUE));

    LONG count = 0;
    hr = copy->get_Count(&count);
    log(L"Copy item count = " + std::to_wstring(count));

    if (SUCCEEDED(hr) && count > 0) {
        log(L"Removing all items except index " + std::to_wstring(updateIndex) + L"...");
        for (LONG i = count - 1; i >= 0; --i) {
            if (i != updateIndex) {
                copy->RemoveAt(i);
            }
        }
        hr = copy->get_Count(&count);
        log(L"After removal, count = " + std::to_wstring(count));
    }

    return copy;
}

void UpdateEngine::setStatusCallback(StatusCallback callback) {
    statusCallback_ = std::move(callback);
}

void UpdateEngine::setLogCallback(LogCallback callback) {
    logCallback_ = std::move(callback);
}

void UpdateEngine::setProgressCallback(ProgressCallback callback) {
    progressCallback_ = std::move(callback);
}

void UpdateEngine::notify(Phase phase, std::wstring_view detail) const {
    if (statusCallback_) {
        statusCallback_(phase, detail);
    }
}

void UpdateEngine::log(std::wstring_view message) const {
    if (logCallback_) {
        logCallback_(message);
    }
}

void UpdateEngine::progress(std::wstring_view title, int percent) const {
    if (progressCallback_) {
        progressCallback_(title, percent);
    }
}

UpdateEngine::StepOutcome UpdateEngine::downloadOne(IUpdateSession* session, IUpdate* update,
                                                     IUpdateCollection* searchResults, LONG updateIndex,
                                                     std::wstring_view title) const {
    StepOutcome outcome;

    ComPtr<IUpdateCollection> collection = makeSingleCollection(update);
    if (!collection) {
        log(L"makeSingleCollection failed, trying fallback via search results Copy...");
        collection = makeCollectionFallback(searchResults, updateIndex);
    }
    if (!collection) {
        outcome.detail = L"Failed to prepare update collection (see log for COM HRESULT details).";
        return outcome;
    }

    ComPtr<IUpdateDownloader> downloader;
    HRESULT hr = session->CreateUpdateDownloader(downloader.put());
    if (FAILED(hr) || !downloader) {
        outcome.detail = L"Failed to create downloader.";
        return outcome;
    }

    log(L"Downloading: " + std::wstring(title));
    outcome = downloadWithProgress(downloader.get(), collection.get(), progress, title);
    if (outcome.ok) {
        log(L"  Download OK.");
    } else {
        log(L"  " + outcome.detail);
    }
    return outcome;
}

UpdateEngine::StepOutcome UpdateEngine::installOne(IUpdateSession* session, IUpdate* update,
                                                    IUpdateCollection* searchResults, LONG updateIndex,
                                                    std::wstring_view title) const {
    StepOutcome outcome;

    ComPtr<IUpdateCollection> collection = makeSingleCollection(update);
    if (!collection) {
        log(L"makeSingleCollection failed, trying fallback via search results Copy...");
        collection = makeCollectionFallback(searchResults, updateIndex);
    }
    if (!collection) {
        outcome.detail = L"Failed to prepare update collection (see log for COM HRESULT details).";
        return outcome;
    }

    ComPtr<IUpdateInstaller> installer;
    HRESULT hr = session->CreateUpdateInstaller(installer.put());
    if (FAILED(hr) || !installer) {
        outcome.detail = L"Failed to create installer.";
        return outcome;
    }

    log(L"Installing: " + std::wstring(title));
    outcome = installWithProgress(installer.get(), collection.get(), progress, title);
    if (outcome.ok) {
        log(L"  Install OK.");
    } else {
        log(L"  " + outcome.detail);
    }
    return outcome;
}

UpdateEngine::Result UpdateEngine::runCycle() {
    Result result;

    ComInitializer com(COINIT_APARTMENTTHREADED);
    if (!com.ok()) {
        result.message = L"Failed to initialize COM.";
        return result;
    }

    if (!ensureWindowsUpdateService()) {
        result.message = L"Windows Update service is not running.";
        return result;
    }

    ComPtr<IUpdateSession> session;
    HRESULT hr = CoCreateInstance(CLSID_UpdateSession, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IUpdateSession, reinterpret_cast<void**>(session.put()));
    if (FAILED(hr) || !session) {
        result.message = L"Failed to create update session (" + formatHresult(hr) + L").";
        return result;
    }

    session->put_ClientApplicationID(BStr(L"Single Update"));

    ComPtr<IUpdateSearcher> searcher;
    hr = session->CreateUpdateSearcher(searcher.put());
    if (FAILED(hr) || !searcher) {
        result.message = L"Failed to create update searcher.";
        return result;
    }

    searcher->put_Online(VARIANT_TRUE);
    searcher->put_ServerSelection(ssWindowsUpdate);
    searcher->put_IncludePotentiallySupersededUpdates(VARIANT_FALSE);

    notify(Phase::Searching, L"Searching updates...");
    log(L"Searching Windows Update catalog...");

    ComPtr<ISearchResult> searchResult;
    hr = searcher->Search(BStr(kSearchCriteria), searchResult.put());
    if (FAILED(hr) || !searchResult) {
        result.message = L"Update search failed (" + formatHresult(hr) + L").";
        return result;
    }

    OperationResultCode searchCode = orcNotStarted;
    searchResult->get_ResultCode(&searchCode);
    if (!operationOk(searchCode)) {
        result.message = L"Update search returned: " + operationResultName(searchCode) + L".";
        return result;
    }

    ComPtr<IUpdateCollection> updates;
    hr = searchResult->get_Updates(updates.put());
    if (FAILED(hr) || !updates) {
        result.message = L"Failed to read update list.";
        return result;
    }

    const long count = collectionCount(updates.get());
    result.updatesFound = static_cast<int>(count);

    if (count == 0) {
        log(L"No pending updates found. Checking for installed updates needing restart...");
        if (hasPendingRestart(session.get())) {
            notify(Phase::RebootRequired, L"Restart required to finish installation.");
            log(L"Updates are installed but pending restart. Reboot needed.");
            result.success = true;
            result.pendingRestart = true;
            result.rebootRequired = true;
            return result;
        }

        notify(Phase::CheckingAgain, L"Final verification...");
        log(L"No updates found. Polling for catalog stabilization...");

        constexpr int kMaxPollSeconds = 10;
        constexpr int kPollIntervalMs = 1000;

        for (int poll = 0; poll < kMaxPollSeconds; ++poll) {
            Sleep(kPollIntervalMs);

            ComPtr<ISearchResult> finalResult;
            hr = searcher->Search(BStr(kSearchCriteria), finalResult.put());
            if (SUCCEEDED(hr) && finalResult) {
                ComPtr<IUpdateCollection> finalUpdates;
                hr = finalResult->get_Updates(finalUpdates.put());
                if (SUCCEEDED(hr) && finalUpdates) {
                    const long finalCount = collectionCount(finalUpdates.get());
                    log(L"  Poll " + std::to_wstring(poll + 1) + L"/" +
                        std::to_wstring(kMaxPollSeconds) + L": " +
                        std::to_wstring(finalCount) + L" update(s).");
                    if (finalCount > 0) {
                        log(L"Updates appeared after stabilization. Continuing update cycle...");
                        result.success = true;
                        return result;
                    }
                }
            }
        }

        result.finalVerificationDone = true;
        notify(Phase::UpToDate, L"System is up to date.");
        log(L"No updates found and no restart pending after final verification.");
        result.success = true;
        result.upToDate = true;
        return result;
    }

    log(L"Found " + std::to_wstring(count) + L" update(s):");
    for (long i = 0; i < count; ++i) {
        log(L"  - " + updateTitle(updates.get(), i));
    }

    long installed = 0;
    long failed = 0;
    long skipped = 0;
    bool rebootRequired = false;
    std::wstring failureLog;

    for (long i = 0; i < count; ++i) {
        ComPtr<IUpdate> update;
        if (FAILED(updates->get_Item(i, update.put())) || !update) {
            ++failed;
            failureLog += L"- Unknown update: could not read entry.\r\n";
            continue;
        }

        const std::wstring title = updateTitle(update.get());

        if (isDefenderRelatedUpdate(title)) {
            notify(Phase::Downloading, L"Skipping Defender update...");
            log(L"Skipped (Windows Defender manages this separately): " + title);
            ++skipped;
            continue;
        }

        if (!acceptEula(update.get())) {
            ++failed;
            failureLog += L"- " + title + L": EULA not accepted.\r\n";
            log(L"FAILED: " + title);
            log(L"  Could not accept EULA.");
            continue;
        }

        notify(Phase::Downloading, L"Downloading...");
        const StepOutcome download = downloadOne(session.get(), update.get(), updates.get(), i, title);
        if (!download.ok) {
            ++failed;
            failureLog += L"- " + title + L" (download): " + download.detail + L"\r\n";
            continue;
        }

        notify(Phase::Installing, L"Installing...");
        const StepOutcome install = installOne(session.get(), update.get(), updates.get(), i, title);
        if (!install.ok) {
            ++failed;
            failureLog += L"- " + title + L" (install): " + install.detail + L"\r\n";
            if (install.rebootRequired) {
                rebootRequired = true;
            }
            continue;
        }

        ++installed;
        if (install.rebootRequired) {
            rebootRequired = true;
        }
    }

    log(L"Summary: " + std::to_wstring(installed) + L" installed, " + std::to_wstring(failed) +
        L" failed, " + std::to_wstring(skipped) + L" skipped.");

    result.updatesInstalled = static_cast<int>(installed);
    result.updatesFailed = static_cast<int>(failed);
    result.updatesSkipped = static_cast<int>(skipped);

    if (rebootRequired) {
        notify(Phase::RebootRequired, L"Restarting...");
        log(L"Restart required. Windows will reboot in 15 seconds...");
        result.success = true;
        result.rebootRequired = true;
        result.hadFailures = failed > 0;
        return result;
    }

    if (installed > 0) {
        notify(Phase::CheckingAgain, L"Checking again...");
        log(L"Checking for more updates...");
        result.success = true;
        result.hadFailures = failed > 0;
        if (failed > 0) {
            log(L"Some updates failed:");
            log(failureLog);
        }
        return result;
    }

    if (skipped > 0 && failed == 0) {
        notify(Phase::UpToDate, L"System is up to date.");
        log(L"Remaining updates are handled by Windows Defender.");
        result.success = true;
        result.upToDate = true;
        return result;
    }

    result.message = L"All updates failed.\r\n\r\n" + failureLog;
    log(L"All updates failed.");
    log(failureLog);
    return result;
}
