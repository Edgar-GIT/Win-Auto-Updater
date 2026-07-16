#include "update.hpp"
#include "com_ptr.hpp"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <wuapi.h>
#include <windows.h>

#include <string>

namespace {

constexpr const wchar_t* kSearchCriteria =
    L"(IsInstalled=0 and IsHidden=0 and Type='Software') or "
    L"(IsInstalled=0 and IsHidden=0 and Type='Driver')";

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

bool rebootRequired(IInstallationResult* result) {
    if (!result) {
        return false;
    }
    VARIANT_BOOL required = VARIANT_FALSE;
    if (FAILED(result->get_RebootRequired(&required))) {
        return false;
    }
    return required == VARIANT_TRUE;
}

}  // namespace

void UpdateEngine::setStatusCallback(StatusCallback callback) {
    callback_ = std::move(callback);
}

void UpdateEngine::notify(Phase phase, std::wstring_view detail) const {
    if (callback_) {
        callback_(phase, detail);
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
    searcher->put_ServerSelection(ssWindowsUpdate);
    searcher->put_IncludePotentiallySupersededUpdates(VARIANT_FALSE);

    notify(Phase::Searching, L"Searching updates...");

    ComPtr<ISearchResult> searchResult;
    hr = searcher->Search(BStr(kSearchCriteria), searchResult.put());
    if (FAILED(hr) || !searchResult) {
        result.message = L"Update search failed.";
        return result;
    }

    OperationResultCode searchCode = orcNotStarted;
    searchResult->get_ResultCode(&searchCode);
    if (searchCode != orcSucceeded && searchCode != orcSucceededWithErrors) {
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
        notify(Phase::UpToDate, L"System is up to date.");
        result.success = true;
        result.upToDate = true;
        return result;
    }

    notify(Phase::Downloading, L"Downloading...");

    ComPtr<IUpdateDownloader> downloader;
    hr = session->CreateUpdateDownloader(downloader.put());
    if (FAILED(hr) || !downloader) {
        result.message = L"Failed to create update downloader.";
        return result;
    }

    downloader->put_Updates(updates.get());
    downloader->put_IsForced(VARIANT_TRUE);

    ComPtr<IDownloadResult> downloadResult;
    hr = downloader->Download(downloadResult.put());
    if (FAILED(hr) || !downloadResult) {
        result.message = L"Update download failed.";
        return result;
    }

    OperationResultCode downloadCode = orcNotStarted;
    downloadResult->get_ResultCode(&downloadCode);
    if (downloadCode != orcSucceeded && downloadCode != orcSucceededWithErrors) {
        result.message = L"Update download returned an error.";
        return result;
    }

    notify(Phase::Installing, L"Installing...");

    ComPtr<IUpdateInstaller> installer;
    hr = session->CreateUpdateInstaller(installer.put());
    if (FAILED(hr) || !installer) {
        result.message = L"Failed to create update installer.";
        return result;
    }

    installer->put_Updates(updates.get());
    installer->put_IsForced(VARIANT_TRUE);
    installer->put_AllowSourcePrompts(VARIANT_FALSE);

    ComPtr<IInstallationResult> installResult;
    hr = installer->Install(installResult.put());
    if (FAILED(hr) || !installResult) {
        result.message = L"Update installation failed.";
        return result;
    }

    OperationResultCode installCode = orcNotStarted;
    installResult->get_ResultCode(&installCode);
    if (installCode != orcSucceeded && installCode != orcSucceededWithErrors) {
        result.message = L"Update installation returned an error.";
        return result;
    }

    if (rebootRequired(installResult.get())) {
        notify(Phase::RebootRequired, L"Restarting...");
        result.success = true;
        result.rebootRequired = true;
        return result;
    }

    notify(Phase::CheckingAgain, L"Checking again...");
    result.success = true;
    return result;
}
