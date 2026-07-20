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

#include <string>

namespace {

constexpr const wchar_t* kSearchCriteria = L"IsInstalled=0 and IsHidden=0";

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

std::wstring updateTitle(IUpdateCollection* collection, LONG index) {
    if (!collection || index < 0) {
        return L"Update";
    }

    ComPtr<IUpdate> update;
    if (FAILED(collection->get_Item(index, update.put())) || !update) {
        return L"Update";
    }

    BStr title;
    if (FAILED(update->get_Title(title.put())) || !title) {
        return L"Update";
    }
    return std::wstring(title.get());
}

std::wstring hresultMessage(HRESULT hr) {
    return L"HRESULT 0x" + std::to_wstring(static_cast<unsigned long>(hr));
}

bool acceptEulas(IUpdateCollection* collection) {
    const long count = collectionCount(collection);
    for (long i = 0; i < count; ++i) {
        ComPtr<IUpdate> update;
        if (FAILED(collection->get_Item(i, update.put())) || !update) {
            continue;
        }

        VARIANT_BOOL accepted = VARIANT_FALSE;
        if (SUCCEEDED(update->get_EulaAccepted(&accepted)) && accepted == VARIANT_FALSE) {
            if (FAILED(update->AcceptEula())) {
                return false;
            }
        }
    }
    return true;
}

bool registryKeyExists(HKEY root, const wchar_t* subKey) {
    HKEY key = nullptr;
    const LONG result = RegOpenKeyExW(root, subKey, 0, KEY_READ, &key);
    if (result == ERROR_SUCCESS) {
        RegCloseKey(key);
        return true;
    }
    return false;
}

bool registryRebootPending() {
    static const wchar_t* keys[] = {
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\RebootRequired",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing\\RebootPending",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\PostRebootReporting",
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\UacRebootRequired",
    };

    for (const wchar_t* key : keys) {
        if (registryKeyExists(HKEY_LOCAL_MACHINE, key)) {
            return true;
        }
    }

    HKEY session = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Control\\Session Manager", 0, KEY_READ,
                      &session) == ERROR_SUCCESS) {
        DWORD type = 0;
        DWORD size = 0;
        const LONG pending =
            RegQueryValueExW(session, L"PendingFileRenameOperations", nullptr, &type, nullptr,
                            &size);
        RegCloseKey(session);
        if (pending == ERROR_SUCCESS && size > 0) {
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

bool systemNeedsReboot() {
    if (registryRebootPending()) {
        return true;
    }

    ComPtr<ISystemInformation> info;
    HRESULT hr = CoCreateInstance(CLSID_SystemInformation, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ISystemInformation, reinterpret_cast<void**>(info.put()));
    if (FAILED(hr) || !info) {
        return false;
    }

    VARIANT_BOOL required = VARIANT_FALSE;
    if (FAILED(info->get_RebootRequired(&required))) {
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

}  // namespace

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
        result.message = L"Failed to create update session. " + hresultMessage(hr);
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
        result.message = L"Update search failed. " + hresultMessage(hr);
        return result;
    }

    OperationResultCode searchCode = orcNotStarted;
    searchResult->get_ResultCode(&searchCode);
    if (!operationOk(searchCode)) {
        result.message = L"Update search returned an error.";
        return result;
    }

    ComPtr<IUpdateCollection> updates;
    hr = searchResult->get_Updates(updates.put());
    if (FAILED(hr) || !updates) {
        result.message = L"Failed to read update list.";
        return result;
    }

    const long count = collectionCount(updates.get());
    if (count == 0) {
        if (systemNeedsReboot()) {
            notify(Phase::RebootRequired, L"Restarting...");
            log(L"Windows is waiting for a restart to finish pending updates.");
            result.success = true;
            result.rebootRequired = true;
            return result;
        }

        notify(Phase::UpToDate, L"System is up to date.");
        log(L"No updates found.");
        result.success = true;
        result.upToDate = true;
        return result;
    }

    log(L"Found " + std::to_wstring(count) + L" update(s):");
    for (long i = 0; i < count; ++i) {
        log(L"  - " + updateTitle(updates.get(), i));
    }

    if (!acceptEulas(updates.get())) {
        result.message = L"Failed to accept update EULAs.";
        return result;
    }

    notify(Phase::Downloading, L"Downloading...");
    log(L"Downloading updates...");

    ComPtr<IUpdateDownloader> downloader;
    hr = session->CreateUpdateDownloader(downloader.put());
    if (FAILED(hr) || !downloader) {
        result.message = L"Failed to create update downloader.";
        return result;
    }

    downloader->put_Updates(updates.get());
    downloader->put_Priority(dpHigh);
    downloader->put_IsForced(VARIANT_TRUE);

    progress(updateTitle(updates.get(), 0), 0);

    ComPtr<IDownloadResult> downloadResult;
    hr = downloader->Download(downloadResult.put());
    if (FAILED(hr) || !downloadResult) {
        result.message = L"Update download failed. " + hresultMessage(hr);
        return result;
    }

    OperationResultCode downloadCode = orcNotStarted;
    downloadResult->get_ResultCode(&downloadCode);
    if (!operationOk(downloadCode)) {
        result.message = L"Update download returned an error.";
        return result;
    }

    progress(updateTitle(updates.get(), count - 1), 100);
    log(L"Download complete.");

    notify(Phase::Installing, L"Installing...");
    log(L"Installing updates (this may take several minutes)...");

    ComPtr<IUpdateInstaller> installer;
    hr = session->CreateUpdateInstaller(installer.put());
    if (FAILED(hr) || !installer) {
        result.message = L"Failed to create update installer.";
        return result;
    }

    installer->put_Updates(updates.get());
    installer->put_IsForced(VARIANT_TRUE);
    installer->put_AllowSourcePrompts(VARIANT_FALSE);

    VARIANT_BOOL rebootBefore = VARIANT_FALSE;
    if (SUCCEEDED(installer->get_RebootRequiredBeforeInstallation(&rebootBefore)) &&
        rebootBefore == VARIANT_TRUE) {
        notify(Phase::RebootRequired, L"Restarting...");
        log(L"Windows requires a restart before installation can continue.");
        result.success = true;
        result.rebootRequired = true;
        return result;
    }

    for (long i = 0; i < count; ++i) {
        const int percent = count <= 1 ? 50 : static_cast<int>((i * 100) / count);
        progress(updateTitle(updates.get(), i), percent);
        log(L"Installing: " + updateTitle(updates.get(), i));
    }

    ComPtr<IInstallationResult> installResult;
    hr = installer->Install(installResult.put());
    if (FAILED(hr) || !installResult) {
        result.message = L"Update installation failed. " + hresultMessage(hr);
        return result;
    }

    OperationResultCode installCode = orcNotStarted;
    installResult->get_ResultCode(&installCode);
    if (!operationOk(installCode)) {
        result.message = L"Update installation returned an error.";
        return result;
    }

    progress(updateTitle(updates.get(), count - 1), 100);
    log(L"Installation complete.");

    if (resultNeedsReboot(installResult.get()) || systemNeedsReboot()) {
        notify(Phase::RebootRequired, L"Restarting...");
        log(L"Restart required. Windows will reboot in 15 seconds...");
        result.success = true;
        result.rebootRequired = true;
        return result;
    }

    notify(Phase::CheckingAgain, L"Checking again...");
    log(L"Checking for more updates...");
    result.success = true;
    return result;
}
