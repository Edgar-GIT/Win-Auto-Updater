#include "scheduler.hpp"
#include "com_ptr.hpp"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <taskschd.h>
#include <windows.h>

#include <string>

namespace {

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

}  // namespace

Scheduler::Scheduler(std::filesystem::path executable)
    : executable_(std::move(executable)) {}

bool Scheduler::exists() const {
    ComInitializer com;
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

bool Scheduler::remove() const {
    ComInitializer com;
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
    return SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

bool Scheduler::create() const {
    ComInitializer com;
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

    folder->DeleteTask(BStr(kTaskName), 0);

    ComPtr<ITaskDefinition> definition;
    hr = service->NewTask(0, definition.put());
    if (FAILED(hr) || !definition) {
        return false;
    }

    ComPtr<IRegistrationInfo> regInfo;
    hr = definition->get_RegistrationInfo(regInfo.put());
    if (SUCCEEDED(hr) && regInfo) {
        regInfo->put_Author(BStr(L"Single Update"));
        regInfo->put_Description(BStr(L"Resumes Single Update after reboot"));
    }

    ComPtr<IPrincipal> principal;
    hr = definition->get_Principal(principal.put());
    if (FAILED(hr) || !principal) {
        return false;
    }

    principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
    principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);

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
    }

    ComPtr<ITriggerCollection> triggers;
    hr = definition->get_Triggers(triggers.put());
    if (FAILED(hr) || !triggers) {
        return false;
    }

    ComPtr<ITrigger> trigger;
    hr = triggers->Create(TASK_TRIGGER_LOGON, trigger.put());
    if (FAILED(hr) || !trigger) {
        return false;
    }

    ComPtr<ILogonTrigger> logonTrigger;
    hr = trigger->QueryInterface(IID_ILogonTrigger, reinterpret_cast<void**>(logonTrigger.put()));
    if (FAILED(hr) || !logonTrigger) {
        return false;
    }

    logonTrigger->put_Id(BStr(L"SingleUpdateLogon"));
    logonTrigger->put_Delay(BStr(L"PT30S"));
    logonTrigger->put_Enabled(VARIANT_TRUE);

    ComPtr<IActionCollection> actions;
    hr = definition->get_Actions(actions.put());
    if (FAILED(hr) || !actions) {
        return false;
    }

    ComPtr<IAction> action;
    hr = actions->Create(TASK_ACTION_EXEC, action.put());
    if (FAILED(hr) || !action) {
        return false;
    }

    ComPtr<IExecAction> execAction;
    hr = action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(execAction.put()));
    if (FAILED(hr) || !execAction) {
        return false;
    }

    const std::wstring path = executable_.wstring();
    hr = execAction->put_Path(BStr(path.c_str()));
    if (FAILED(hr)) {
        return false;
    }

    if (executable_.has_parent_path()) {
        execAction->put_WorkingDirectory(BStr(executable_.parent_path().wstring().c_str()));
    }

    Variant emptyUser;
    Variant emptyPassword;
    Variant sddl(L"");

    ComPtr<IRegisteredTask> registered;
    hr = folder->RegisterTaskDefinition(BStr(kTaskName), definition.get(), TASK_CREATE_OR_UPDATE,
                                        emptyUser, emptyPassword, TASK_LOGON_INTERACTIVE_TOKEN,
                                        sddl, registered.put());

    return SUCCEEDED(hr) && registered;
}
