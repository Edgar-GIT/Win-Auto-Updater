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
#include <string>

namespace {

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

ComPtr<IUpdateCollection> makeSingleCollection(IUpdate* update) {
    ComPtr<IUpdateCollection> collection;
    HRESULT hr = CoCreateInstance(CLSID_UpdateCollection, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IUpdateCollection, reinterpret_cast<void**>(collection.put()));
    if (FAILED(hr) || !collection || !update) {
        return {};
    }

    collection->Clear();
    LONG index = 0;
    if (FAILED(collection->Add(update, &index))) {
        return {};
    }
    return collection;
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

UpdateEngine::StepOutcome UpdateEngine::downloadOne(IUpdateSession* session, IUpdate* update,
                                                    std::wstring_view title) const {
    StepOutcome outcome;

    ComPtr<IUpdateCollection> collection = makeSingleCollection(update);
    if (!collection) {
        outcome.detail = L"Failed to prepare update collection.";
        return outcome;
    }

    ComPtr<IUpdateDownloader> downloader;
    HRESULT hr = session->CreateUpdateDownloader(downloader.put());
    if (FAILED(hr) || !downloader) {
        outcome.detail = L"Failed to create downloader.";
        return outcome;
    }

    downloader->put_Updates(collection.get());
    downloader->put_Priority(dpHigh);
    downloader->put_IsForced(VARIANT_TRUE);

    progress(title, 10);
    log(L"Downloading: " + std::wstring(title));

    ComPtr<IDownloadResult> result;
    hr = downloader->Download(result.put());
    if (FAILED(hr) || !result) {
        outcome.detail = L"Download API failed (" + formatHresult(hr) + L").";
        return outcome;
    }

    OperationResultCode code = orcNotStarted;
    result->get_ResultCode(&code);

    ComPtr<IUpdateDownloadResult> updateResult;
    if (SUCCEEDED(result->GetUpdateResult(0, updateResult.put())) && updateResult) {
        LONG updateHr = 0;
        OperationResultCode updateCode = orcNotStarted;
        updateResult->get_HResult(&updateHr);
        updateResult->get_ResultCode(&updateCode);

        if (!operationOk(updateCode)) {
            outcome.detail = L"Download failed: " + operationResultName(updateCode) + L" (" +
                             formatHresult(updateHr) + L").";
            log(L"  " + outcome.detail);
            return outcome;
        }
    } else if (!operationOk(code)) {
        LONG overallHr = 0;
        result->get_HResult(&overallHr);
        outcome.detail = L"Download failed: " + operationResultName(code) + L" (" +
                         formatHresult(overallHr) + L").";
        log(L"  " + outcome.detail);
        return outcome;
    }

    progress(title, 100);
    log(L"  Download OK.");
    outcome.ok = true;
    return outcome;
}

UpdateEngine::StepOutcome UpdateEngine::installOne(IUpdateSession* session, IUpdate* update,
                                                   std::wstring_view title) const {
    StepOutcome outcome;

    ComPtr<IUpdateCollection> collection = makeSingleCollection(update);
    if (!collection) {
        outcome.detail = L"Failed to prepare update collection.";
        return outcome;
    }

    ComPtr<IUpdateInstaller> installer;
    HRESULT hr = session->CreateUpdateInstaller(installer.put());
    if (FAILED(hr) || !installer) {
        outcome.detail = L"Failed to create installer.";
        return outcome;
    }

    installer->put_Updates(collection.get());
    installer->put_IsForced(VARIANT_TRUE);
    installer->put_AllowSourcePrompts(VARIANT_FALSE);

    VARIANT_BOOL rebootBefore = VARIANT_FALSE;
    if (SUCCEEDED(installer->get_RebootRequiredBeforeInstallation(&rebootBefore)) &&
        rebootBefore == VARIANT_TRUE) {
        outcome.rebootRequired = true;
        outcome.detail = L"Windows requires reboot before this update.";
        return outcome;
    }

    progress(title, 10);
    log(L"Installing: " + std::wstring(title));

    ComPtr<IInstallationResult> result;
    hr = installer->Install(result.put());
    if (FAILED(hr) || !result) {
        outcome.detail = L"Install API failed (" + formatHresult(hr) + L").";
        return outcome;
    }

    OperationResultCode code = orcNotStarted;
    result->get_ResultCode(&code);

    ComPtr<IUpdateInstallationResult> updateResult;
    if (SUCCEEDED(result->GetUpdateResult(0, updateResult.put())) && updateResult) {
        LONG updateHr = 0;
        OperationResultCode updateCode = orcNotStarted;
        VARIANT_BOOL reboot = VARIANT_FALSE;
        updateResult->get_HResult(&updateHr);
        updateResult->get_ResultCode(&updateCode);
        updateResult->get_RebootRequired(&reboot);

        if (!operationOk(updateCode)) {
            outcome.detail = L"Install failed: " + operationResultName(updateCode) + L" (" +
                             formatHresult(updateHr) + L").";
            log(L"  " + outcome.detail);
            return outcome;
        }

        if (reboot == VARIANT_TRUE || resultNeedsReboot(result.get())) {
            outcome.rebootRequired = true;
        }
    } else if (!operationOk(code)) {
        LONG overallHr = 0;
        result->get_HResult(&overallHr);
        outcome.detail = L"Install failed: " + operationResultName(code) + L" (" +
                         formatHresult(overallHr) + L").";
        log(L"  " + outcome.detail);
        return outcome;
    } else if (resultNeedsReboot(result.get())) {
        outcome.rebootRequired = true;
    }

    progress(title, 100);
    log(L"  Install OK.");
    outcome.ok = true;
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
    if (count == 0) {
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
        const StepOutcome download = downloadOne(session.get(), update.get(), title);
        if (!download.ok) {
            ++failed;
            failureLog += L"- " + title + L" (download): " + download.detail + L"\r\n";
            continue;
        }

        notify(Phase::Installing, L"Installing...");
        const StepOutcome install = installOne(session.get(), update.get(), title);
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
