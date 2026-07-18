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

#include <atomic>
#include <functional>
#include <string>

namespace {

constexpr const wchar_t* kSearchCriteria = L"IsInstalled=0 and IsHidden=0";

DEFINE_GUID(IID_IDownloadProgressChangedCallback, 0x8c3f1cdd, 0x6173, 0x4591, 0xae, 0xbd, 0xa5,
            0x6a, 0x53, 0xca, 0x77, 0xc1);
DEFINE_GUID(IID_IDownloadCompletedCallback, 0x77254866, 0x9f5b, 0x4c8e, 0xb9, 0xe2, 0xc7, 0x7a,
            0x85, 0x30, 0xd6, 0x4b);
DEFINE_GUID(IID_IInstallationProgressChangedCallback, 0xe01402d5, 0xf8da, 0x43ba, 0xa0, 0x12, 0x38,
            0x89, 0x4b, 0xd0, 0x48, 0xf1);
DEFINE_GUID(IID_IInstallationCompletedCallback, 0x45f4f6f3, 0xd602, 0x4f98, 0x9a, 0x8a, 0x3e, 0xfa,
            0x15, 0x2a, 0xd2, 0xd3);

MIDL_INTERFACE("8c3f1cdd-6173-4591-aebd-a56a53ca77c1")
IDownloadProgressChangedCallback : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Invoke(IDownloadJob* downloadJob, IUnknown* callbackArgs) = 0;
};

MIDL_INTERFACE("77254866-9f5b-4c8e-b9e2-c77a8530d64b")
IDownloadCompletedCallback : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Invoke(IDownloadJob* downloadJob, IUnknown* callbackArgs) = 0;
};

MIDL_INTERFACE("e01402d5-f8da-43ba-a012-38894bd048f1")
IInstallationProgressChangedCallback : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Invoke(IInstallationJob* installationJob,
                                             IUnknown* callbackArgs) = 0;
};

MIDL_INTERFACE("45f4f6f3-d602-4f98-9a8a-3efa152ad2d3")
IInstallationCompletedCallback : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Invoke(IInstallationJob* installationJob,
                                             IUnknown* callbackArgs) = 0;
};

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

class CompletionSignal {
public:
    CompletionSignal() : event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    ~CompletionSignal() {
        if (event_) {
            CloseHandle(event_);
        }
    }

    CompletionSignal(const CompletionSignal&) = delete;
    CompletionSignal& operator=(const CompletionSignal&) = delete;

    void signal() {
        if (event_) {
            SetEvent(event_);
        }
    }

    bool wait(DWORD timeoutMs) const {
        return event_ && WaitForSingleObject(event_, timeoutMs) == WAIT_OBJECT_0;
    }

private:
    HANDLE event_;
};


class DownloadProgressChangedCallback final : public IDownloadProgressChangedCallback {
public:
    using Fn = std::function<void(IDownloadJob*)>;

    explicit DownloadProgressChangedCallback(Fn fn) : fn_(std::move(fn)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IDownloadProgressChangedCallback) {
            *ppv = static_cast<IDownloadProgressChangedCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++ref_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG value = --ref_;
        if (value == 0) {
            delete this;
        }
        return value;
    }

    HRESULT STDMETHODCALLTYPE Invoke(IDownloadJob* job, IUnknown*) override {
        if (fn_) {
            fn_(job);
        }
        return S_OK;
    }

private:
    Fn fn_;
    std::atomic<ULONG> ref_{1};
};

class DownloadCompletedCallback final : public IDownloadCompletedCallback {
public:
    explicit DownloadCompletedCallback(CompletionSignal& signal) : signal_(signal) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IDownloadCompletedCallback) {
            *ppv = static_cast<IDownloadCompletedCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++ref_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG value = --ref_;
        if (value == 0) {
            delete this;
        }
        return value;
    }

    HRESULT STDMETHODCALLTYPE Invoke(IDownloadJob*, IUnknown*) override {
        signal_.signal();
        return S_OK;
    }

private:
    CompletionSignal& signal_;
    std::atomic<ULONG> ref_{1};
};

class InstallationProgressChangedCallback final : public IInstallationProgressChangedCallback {
public:
    using Fn = std::function<void(IInstallationJob*)>;

    explicit InstallationProgressChangedCallback(Fn fn) : fn_(std::move(fn)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IInstallationProgressChangedCallback) {
            *ppv = static_cast<IInstallationProgressChangedCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++ref_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG value = --ref_;
        if (value == 0) {
            delete this;
        }
        return value;
    }

    HRESULT STDMETHODCALLTYPE Invoke(IInstallationJob* job, IUnknown*) override {
        if (fn_) {
            fn_(job);
        }
        return S_OK;
    }

private:
    Fn fn_;
    std::atomic<ULONG> ref_{1};
};

class InstallationCompletedCallback final : public IInstallationCompletedCallback {
public:
    explicit InstallationCompletedCallback(CompletionSignal& signal) : signal_(signal) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IInstallationCompletedCallback) {
            *ppv = static_cast<IInstallationCompletedCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++ref_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG value = --ref_;
        if (value == 0) {
            delete this;
        }
        return value;
    }

    HRESULT STDMETHODCALLTYPE Invoke(IInstallationJob*, IUnknown*) override {
        signal_.signal();
        return S_OK;
    }

private:
    CompletionSignal& signal_;
    std::atomic<ULONG> ref_{1};
};

void reportDownloadProgress(IDownloadJob* job, IUpdateCollection* updates,
                            const UpdateEngine::ProgressCallback& callback) {
    if (!job || !callback) {
        return;
    }

    ComPtr<IDownloadProgress> progressObj;
    if (FAILED(job->GetProgress(progressObj.put())) || !progressObj) {
        return;
    }

    LONG index = 0;
    LONG percent = 0;
    progressObj->get_CurrentUpdateIndex(&index);
    progressObj->get_PercentComplete(&percent);
    callback(updateTitle(updates, index), static_cast<int>(percent));
}

void reportInstallProgress(IInstallationJob* job, IUpdateCollection* updates,
                           const UpdateEngine::ProgressCallback& callback) {
    if (!job || !callback) {
        return;
    }

    ComPtr<IInstallationProgress> progressObj;
    if (FAILED(job->GetProgress(progressObj.put())) || !progressObj) {
        return;
    }

    LONG index = 0;
    LONG percent = 0;
    progressObj->get_CurrentUpdateIndex(&index);
    progressObj->get_PercentComplete(&percent);
    callback(updateTitle(updates, index), static_cast<int>(percent));
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

    ComInitializer com;
    if (!com.ok()) {
        result.message = L"Failed to initialize COM.";
        return result;
    }

    ComPtr<IUpdateSession> session;
    HRESULT hr = CoCreateInstance(CLSID_UpdateSession, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IUpdateSession, reinterpret_cast<void**>(session.put()));
    if (FAILED(hr) || !session) {
        result.message = L"Failed to create update session.";
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
    searcher->put_ServerSelection(ssDefault);
    searcher->put_IncludePotentiallySupersededUpdates(VARIANT_FALSE);

    notify(Phase::Searching, L"Searching updates...");
    log(L"Searching Windows Update catalog...");

    ComPtr<ISearchResult> searchResult;
    hr = searcher->Search(BStr(kSearchCriteria), searchResult.put());
    if (FAILED(hr) || !searchResult) {
        result.message = L"Update search failed.";
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
            log(L"A reboot is required to finish pending updates.");
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
    downloader->put_IsForced(VARIANT_TRUE);

    CompletionSignal downloadDone;
    auto* downloadProgressCb = new DownloadProgressChangedCallback(
        [this, collection = updates.get()](IDownloadJob* job) {
            reportDownloadProgress(job, collection, progressCallback_);
        });
    auto* downloadCompletedCb = new DownloadCompletedCallback(downloadDone);

    VARIANT state{};
    VariantInit(&state);

    ComPtr<IDownloadJob> downloadJob;
    hr = downloader->BeginDownload(downloadProgressCb, downloadCompletedCb, state,
                                   downloadJob.put());
    downloadProgressCb->Release();
    downloadCompletedCb->Release();

    if (FAILED(hr) || !downloadJob) {
        result.message = L"Failed to start update download.";
        return result;
    }

    while (!downloadDone.wait(400)) {
        reportDownloadProgress(downloadJob.get(), updates.get(), progressCallback_);
    }
    reportDownloadProgress(downloadJob.get(), updates.get(), progressCallback_);

    ComPtr<IDownloadResult> downloadResult;
    hr = downloader->EndDownload(downloadJob.get(), downloadResult.put());
    downloadJob->CleanUp();

    if (FAILED(hr) || !downloadResult) {
        result.message = L"Update download failed.";
        return result;
    }

    OperationResultCode downloadCode = orcNotStarted;
    downloadResult->get_ResultCode(&downloadCode);
    if (!operationOk(downloadCode)) {
        result.message = L"Update download returned an error.";
        return result;
    }

    log(L"Download complete.");
    notify(Phase::Installing, L"Installing...");
    log(L"Installing updates...");

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
        log(L"Reboot required before installation can continue.");
        result.success = true;
        result.rebootRequired = true;
        return result;
    }

    CompletionSignal installDone;
    auto* installProgressCb = new InstallationProgressChangedCallback(
        [this, collection = updates.get()](IInstallationJob* job) {
            reportInstallProgress(job, collection, progressCallback_);
        });
    auto* installCompletedCb = new InstallationCompletedCallback(installDone);

    ComPtr<IInstallationJob> installJob;
    hr = installer->BeginInstall(installProgressCb, installCompletedCb, state, installJob.put());
    installProgressCb->Release();
    installCompletedCb->Release();

    if (FAILED(hr) || !installJob) {
        result.message = L"Failed to start update installation.";
        return result;
    }

    while (!installDone.wait(400)) {
        reportInstallProgress(installJob.get(), updates.get(), progressCallback_);
    }
    reportInstallProgress(installJob.get(), updates.get(), progressCallback_);

    ComPtr<IInstallationResult> installResult;
    hr = installer->EndInstall(installJob.get(), installResult.put());
    installJob->CleanUp();

    if (FAILED(hr) || !installResult) {
        result.message = L"Update installation failed.";
        return result;
    }

    OperationResultCode installCode = orcNotStarted;
    installResult->get_ResultCode(&installCode);
    if (!operationOk(installCode)) {
        result.message = L"Update installation returned an error.";
        return result;
    }

    log(L"Installation complete.");

    if (resultNeedsReboot(installResult.get()) || systemNeedsReboot()) {
        notify(Phase::RebootRequired, L"Restarting...");
        log(L"Reboot required. Restarting Windows...");
        result.success = true;
        result.rebootRequired = true;
        return result;
    }

    notify(Phase::CheckingAgain, L"Checking again...");
    log(L"Checking for more updates...");
    result.success = true;
    return result;
}
