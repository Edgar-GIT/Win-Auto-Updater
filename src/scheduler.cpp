#include "scheduler.hpp"
#include "com_ptr.hpp"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define SECURITY_WIN32

#include <lmcons.h>
#include <secext.h>
#include <taskschd.h>
#include <windows.h>

#include <cstdio>
#include <string>

namespace {

constexpr const wchar_t* kRunOnceKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
constexpr const wchar_t* kRunOnceValue = L"WinAutoUpdater";

class Variant {
public:
    Variant() {
        VariantInit(&value_);
    }

    explicit Variant(const wchar_t* text) {
        VariantInit(&value_);
        value_.vt = VT_BSTR;
        value_.bstrVal = SysAllocString(text);
    }

    ~Variant() {
        VariantClear(&value_);
    }

    Variant(const Variant&) = delete;
    Variant& operator=(const Variant&) = delete;

    VARIANT& get() noexcept {
        return value_;
    }

    operator VARIANT&() noexcept {
        return value_;
    }

private:
    VARIANT value_{};
};

bool connectService(ComPtr<ITaskService>& service) {
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITaskService, reinterpret_cast<void**>(service.put()));
    if (FAILED(hr) || !service) {
        return false;
    }

    Variant empty;
    hr = service->Connect(empty, empty, empty, empty);
    return SUCCEEDED(hr);
}

std::wstring currentUserSam() {
    wchar_t buffer[512]{};
    ULONG size = static_cast<ULONG>(sizeof(buffer) / sizeof(buffer[0]));
    if (GetUserNameExW(NameSamCompatible, buffer, &size) && buffer[0] != L'\0') {
        return buffer;
    }

    wchar_t fallback[UNLEN + 1]{};
    DWORD len = UNLEN + 1;
    if (GetUserNameW(fallback, &len) && fallback[0] != L'\0') {
        return fallback;
    }
    return {};
}

void setError(std::wstring* error, const std::wstring& message) {
    if (error) {
        *error = message;
    }
}

std::wstring formatHr(HRESULT hr) {
    wchar_t buffer[32]{};
    swprintf(buffer, 32, L"0x%08X", static_cast<unsigned long>(hr));
    return buffer;
}

}  // namespace

Scheduler::Scheduler(std::filesystem::path executable)
    : executable_(std::move(executable)) {}

bool Scheduler::exists() const {
    ComInitializer com(COINIT_MULTITHREADED);
    if (!com.ok()) {
        return false;
    }

    ComPtr<ITaskService> service;
    if (!connectService(service)) {
        return false;
    }

    ComPtr<ITaskFolder> folder;
    HRESULT hr = service->GetFolder(BStr(L"\\"), folder.put());
    if (FAILED(hr) || !folder) {
        return false;
    }

    ComPtr<IRegisteredTask> task;
    hr = folder->GetTask(BStr(kTaskName), task.put());
    return SUCCEEDED(hr) && task;
}

bool Scheduler::removeRunOnce() const {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunOnceKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return true;
    }
    const LSTATUS status = RegDeleteValueW(key, kRunOnceValue);
    RegCloseKey(key);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

bool Scheduler::remove() const {
    removeRunOnce();

    ComInitializer com(COINIT_MULTITHREADED);
    if (!com.ok()) {
        return false;
    }

    ComPtr<ITaskService> service;
    if (!connectService(service)) {
        return false;
    }

    ComPtr<ITaskFolder> folder;
    HRESULT hr = service->GetFolder(BStr(L"\\"), folder.put());
    if (FAILED(hr) || !folder) {
        return false;
    }

    hr = folder->DeleteTask(BStr(kTaskName), 0);
    folder->DeleteTask(BStr(kLegacyTaskName), 0);
    return SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

bool Scheduler::createRunOnce(std::wstring* error) const {
    if (executable_.empty()) {
        setError(error, L"Executable path is empty.");
        return false;
    }

    HKEY key = nullptr;
    const LSTATUS openStatus =
        RegCreateKeyExW(HKEY_CURRENT_USER, kRunOnceKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr);
    if (openStatus != ERROR_SUCCESS) {
        setError(error, L"Failed to open HKCU RunOnce (" + std::to_wstring(openStatus) + L").");
        return false;
    }

    // Quote the path so spaces are handled when Windows launches RunOnce entries.
    const std::wstring command = L"\"" + executable_.wstring() + L"\"";
    const LSTATUS setStatus =
        RegSetValueExW(key, kRunOnceValue, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(command.c_str()),
                       static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);

    if (setStatus != ERROR_SUCCESS) {
        setError(error, L"Failed to write HKCU RunOnce (" + std::to_wstring(setStatus) + L").");
        return false;
    }
    return true;
}

bool Scheduler::createTask(std::wstring* error) const {
    ComInitializer com(COINIT_MULTITHREADED);
    if (!com.ok()) {
        setError(error, L"COM init failed for Task Scheduler.");
        return false;
    }

    const std::wstring user = currentUserSam();
    if (user.empty()) {
        setError(error, L"Could not determine the current Windows user.");
        return false;
    }

    ComPtr<ITaskService> service;
    if (!connectService(service)) {
        setError(error, L"Failed to connect to Task Scheduler service.");
        return false;
    }

    ComPtr<ITaskFolder> folder;
    HRESULT hr = service->GetFolder(BStr(L"\\"), folder.put());
    if (FAILED(hr) || !folder) {
        setError(error, L"Failed to open Task Scheduler root folder (" + formatHr(hr) + L").");
        return false;
    }

    folder->DeleteTask(BStr(kTaskName), 0);

    ComPtr<ITaskDefinition> definition;
    hr = service->NewTask(0, definition.put());
    if (FAILED(hr) || !definition) {
        setError(error, L"Failed to create task definition (" + formatHr(hr) + L").");
        return false;
    }

    ComPtr<IRegistrationInfo> regInfo;
    hr = definition->get_RegistrationInfo(regInfo.put());
    if (SUCCEEDED(hr) && regInfo) {
        regInfo->put_Author(BStr(L"Win Auto Updater"));
        regInfo->put_Description(
            BStr(L"Resumes Win Auto Updater after reboot so Windows updates can continue."));
    }

    ComPtr<IPrincipal> principal;
    hr = definition->get_Principal(principal.put());
    if (FAILED(hr) || !principal) {
        setError(error, L"Failed to configure task principal (" + formatHr(hr) + L").");
        return false;
    }

    // Run as the interactive user with highest privileges (no UAC prompt when launched by Task Scheduler).
    principal->put_UserId(BStr(user.c_str()));
    principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
    principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);

    ComPtr<ITaskSettings> settings;
    hr = definition->get_Settings(settings.put());
    if (SUCCEEDED(hr) && settings) {
        settings->put_StartWhenAvailable(VARIANT_TRUE);
        settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        settings->put_AllowDemandStart(VARIANT_TRUE);
        settings->put_Enabled(VARIANT_TRUE);
        settings->put_Hidden(VARIANT_FALSE);
        settings->put_ExecutionTimeLimit(BStr(L"PT0S"));
        settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
        settings->put_AllowHardTerminate(VARIANT_TRUE);
        settings->put_Priority(4);
    }

    ComPtr<ITriggerCollection> triggers;
    hr = definition->get_Triggers(triggers.put());
    if (FAILED(hr) || !triggers) {
        setError(error, L"Failed to create task triggers (" + formatHr(hr) + L").");
        return false;
    }

    // Logon trigger — resumes after reboot when the user signs in.
    // (Boot trigger is intentionally omitted: it does not work reliably with
    // TASK_LOGON_INTERACTIVE_TOKEN and can prevent the task from registering/running.)
    {
        ComPtr<ITrigger> trigger;
        hr = triggers->Create(TASK_TRIGGER_LOGON, trigger.put());
        if (FAILED(hr) || !trigger) {
            setError(error, L"Failed to create logon trigger (" + formatHr(hr) + L").");
            return false;
        }

        ComPtr<ILogonTrigger> logonTrigger;
        hr = trigger->QueryInterface(IID_ILogonTrigger, reinterpret_cast<void**>(logonTrigger.put()));
        if (FAILED(hr) || !logonTrigger) {
            setError(error, L"Failed to query ILogonTrigger (" + formatHr(hr) + L").");
            return false;
        }

        logonTrigger->put_Id(BStr(L"WinAutoUpdaterLogon"));
        logonTrigger->put_UserId(BStr(user.c_str()));
        logonTrigger->put_Delay(BStr(L"PT20S"));
        logonTrigger->put_Enabled(VARIANT_TRUE);
    }

    ComPtr<IActionCollection> actions;
    hr = definition->get_Actions(actions.put());
    if (FAILED(hr) || !actions) {
        setError(error, L"Failed to create task actions (" + formatHr(hr) + L").");
        return false;
    }

    ComPtr<IAction> action;
    hr = actions->Create(TASK_ACTION_EXEC, action.put());
    if (FAILED(hr) || !action) {
        setError(error, L"Failed to create exec action (" + formatHr(hr) + L").");
        return false;
    }

    ComPtr<IExecAction> execAction;
    hr = action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(execAction.put()));
    if (FAILED(hr) || !execAction) {
        setError(error, L"Failed to query IExecAction (" + formatHr(hr) + L").");
        return false;
    }

    const std::wstring path = executable_.wstring();
    hr = execAction->put_Path(BStr(path.c_str()));
    if (FAILED(hr)) {
        setError(error, L"Failed to set executable path (" + formatHr(hr) + L").");
        return false;
    }

    if (executable_.has_parent_path()) {
        execAction->put_WorkingDirectory(BStr(executable_.parent_path().wstring().c_str()));
    }

    Variant emptyUser;
    Variant emptyPassword;
    Variant emptySddl;  // VT_EMPTY — do not pass an empty BSTR

    ComPtr<IRegisteredTask> registered;
    hr = folder->RegisterTaskDefinition(BStr(kTaskName), definition.get(), TASK_CREATE_OR_UPDATE,
                                        emptyUser, emptyPassword, TASK_LOGON_INTERACTIVE_TOKEN,
                                        emptySddl, registered.put());
    if (FAILED(hr) || !registered) {
        setError(error, L"RegisterTaskDefinition failed (" + formatHr(hr) + L") for user " + user +
                            L".");
        return false;
    }

    return true;
}

bool Scheduler::armReopen(std::wstring* error) const {
    return createRunOnce(error);
}

bool Scheduler::create(std::wstring* error) const {
    std::wstring taskError;
    const bool taskOk = createTask(&taskError);

    std::wstring runOnceError;
    const bool runOnceOk = createRunOnce(&runOnceError);

    if (taskOk && runOnceOk) {
        return true;
    }

    // One persistence mechanism is enough to resume after reboot.
    if (taskOk || runOnceOk) {
        if (error) {
            if (!taskOk) {
                *error = L"Task Scheduler failed (" + taskError +
                         L") but RunOnce backup was created successfully.";
            } else {
                *error = L"RunOnce backup failed (" + runOnceError +
                         L") but Task Scheduler task was created successfully.";
            }
        }
        return true;
    }

    setError(error, L"Task Scheduler: " + taskError + L" | RunOnce: " + runOnceError);
    return false;
}
