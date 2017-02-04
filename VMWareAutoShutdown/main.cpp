#include <atomic>
#include <cstddef>
#include <functional>
#include <ios>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <Windows.h>
#include <tchar.h>

#include "gflags/gflags.h"
#include "glog/logging.h"
#include "vix.h"
#include "vm_basic_types.h"

DEFINE_bool(debug, false, "In debug mode, it will shutdown all VMs immediately");

class ScopedExit {
public:
    ScopedExit(std::function<void()> func) : func_(func) {}
    ~ScopedExit() { if (func_) func_(); }
private:
    std::function<void()> func_;
};

std::string GetLastErrorMessage();

std::string ToString(const char* str, int len) { return std::string(str, len); }
std::string ToString(const char* str) { return str; }

std::string ToString(const wchar_t* str, int len = -1) {
    std::string out;
    if (len > 0 || len == -1) {
        int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, str, len, nullptr, 0, nullptr, nullptr);
        out.resize(size);
        int ret = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, str, len, &out[0], size, nullptr, nullptr);
        if (size != ret) {
            LOG(ERROR) << "Conversion failed: " << GetLastErrorMessage();
            out.clear();
        } else if (len == -1) {
            out.resize(size - 1);
        }
    }
    return out;
}

template <typename T>
std::pair<std::vector<char*>, std::vector<std::string>> ToArgv(int argc, const T* argv) {
    std::vector<char*> args(argc);
    std::vector<std::string> holder(argc);
    for (int i = 0; i < argc; i++) {
        holder[i] = ToString(argv[i]);
        args[i] = const_cast<char*>(holder[i].c_str());
    }
    return { std::move(args), std::move(holder) };
}

struct JobData {
    VixHandle hostHandle;
    std::mutex mutex;
    std::vector<std::thread> jobs;
};

void HandleRunningVm(VixHandle jobHandle,
                     VixEventType eventType,
                     VixHandle moreEventInfo,
                     void *clientData) {
    VLOG(2) << __FUNCTION__ << ": eventType=" << eventType;
    if (eventType != VIX_EVENTTYPE_FIND_ITEM) {
        return;
    }
    char* location = nullptr;
    ScopedExit freeUrl([&location] { Vix_FreeBuffer(location); });
    VixError err = Vix_GetProperties(
        moreEventInfo,
        VIX_PROPERTY_FOUND_ITEM_LOCATION,
        &location,
        VIX_PROPERTY_NONE);
    if (VIX_FAILED(err)) {
        LOG(ERROR) << "Failed to get VM location: " << err;
        return;
    }
    LOG(INFO) << "Found running VM: " << location;

    JobData* jobData = reinterpret_cast<JobData*>(clientData);
    std::lock_guard<std::mutex> guard(jobData->mutex);
    jobData->jobs.emplace_back([jobData, locationStr = std::string(location), id = jobData->jobs.size()] {
        LOG(INFO) << "Start poweroff job #" << id << ": " << locationStr;
        ScopedExit logJobExit([id] { LOG(INFO) << "Quit poweroff job #" << id; });
        VixHandle vmHandle = VIX_INVALID_HANDLE;
        ScopedExit releaseVmHandle([&vmHandle] { Vix_ReleaseHandle(vmHandle); });

        {
            LOG(INFO) << "Opening VM #" << id;
            VixHandle jobHandle = VixVM_Open(jobData->hostHandle, locationStr.c_str(), nullptr, nullptr);
            ScopedExit releaseJobHandle([jobHandle] { Vix_ReleaseHandle(jobHandle); });
            VLOG(2) << "Job handle 0x" << jobHandle;

            VixError err = VixJob_Wait(jobHandle, VIX_PROPERTY_JOB_RESULT_HANDLE, &vmHandle, VIX_PROPERTY_NONE);
            if (VIX_FAILED(err)) {
                LOG(ERROR) << "Failed to open VM #" << id << ": " << err;
                return;
            }
            VLOG(1) << "VM handle 0x" << vmHandle;
        }

        {
            LOG(INFO) << "Powering off VM #" << id;
            VixHandle jobHandle = VixVM_PowerOff(vmHandle, VIX_VMPOWEROP_FROM_GUEST, nullptr, nullptr);
            ScopedExit releaseJobHandle([jobHandle] { Vix_ReleaseHandle(jobHandle); });
            VLOG(2) << "Job handle 0x" << jobHandle;

            VixError err = VixJob_Wait(jobHandle, VIX_PROPERTY_NONE);
            if (VIX_FAILED(err)) {
                LOG(ERROR) << "Failed to power off VM #" << id << ": " << err;
                return;
            }
        }
    });
}

void ShutdownAllVirtualMachines() {
    VixHandle hostHandle = VIX_INVALID_HANDLE;
    ScopedExit disconnectHost([&hostHandle] { VixHost_Disconnect(hostHandle); });

    {
        LOG(INFO) << "Connecting to localhost";
        VixHandle jobHandle = VixHost_Connect(
            VIX_API_VERSION, VIX_SERVICEPROVIDER_VMWARE_WORKSTATION,
            nullptr, 0, nullptr, nullptr,
            0, VIX_INVALID_HANDLE, nullptr, nullptr);
        ScopedExit releaseJobHandle([jobHandle]() { Vix_ReleaseHandle(jobHandle); });
        VLOG(2) << "Job handle 0x" << jobHandle;

        VixError err = VixJob_Wait(jobHandle, VIX_PROPERTY_JOB_RESULT_HANDLE, &hostHandle, VIX_PROPERTY_NONE);
        if (VIX_FAILED(err)) {
            LOG(ERROR) << "Failed to connect to localhost: " << err;
            return;
        }
        VLOG(1) << "Host handle 0x" << hostHandle;
    }

    JobData jobData;
    jobData.hostHandle = hostHandle;
    {
        LOG(INFO) << "Finding running VMs";
        VixHandle jobHandle = VixHost_FindItems(
            hostHandle,VIX_FIND_RUNNING_VMS, VIX_INVALID_HANDLE, -1, &HandleRunningVm, &jobData);
        ScopedExit releaseJobHandle([jobHandle] { Vix_ReleaseHandle(jobHandle); });
        VLOG(2) << "Job handle 0x" << jobHandle;

        VixError err = VixJob_Wait(jobHandle, VIX_PROPERTY_NONE);
        if (VIX_FAILED(err)) {
            LOG(ERROR) << "Failed to find running VMs: " << err;
            return;
        }
    }

    LOG(INFO) << "Waiting for all jobs to finish";
    std::vector<std::thread> jobs;
    {
        std::lock_guard<std::mutex> guard(jobData.mutex);
        jobs = std::move(jobData.jobs);
    }
    for (std::thread& job : jobs) {
        job.join();
    }
    LOG(INFO) << "All jobs quits";
}

std::string GetLastErrorMessage() {
    LPTSTR errorMessage = nullptr;
    ScopedExit cleanup([&errorMessage] { LocalFree(errorMessage); });
    DWORD code = GetLastError();
    DWORD length = FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
        nullptr, code, 0, reinterpret_cast<LPTSTR>(&errorMessage), 0, nullptr);
    std::stringstream ss;
    if (length <= 0) {
        ss << "error code 0x" << std::hex << code;
    } else {
        ss << "(0x" << std::hex << code << ") " << ToString(errorMessage, length);
    }
    return ss.str();
}

namespace {
std::atomic<bool> quit;
}  // namespace

LRESULT CALLBACK EventHandler(
    _In_ HWND    hWnd,
    _In_ UINT    msg,
    _In_ WPARAM  wParam,
    _In_ LPARAM  lParam) {
    VLOG(2) << std::hex
        << "Message received: hWnd=0x" << (void*)hWnd << ", "
        << "msg=0x" << msg << ", "
        << "wParam=0x" << wParam << ", "
        << "lParam=0x" << lParam;
    if (msg == WM_ENDSESSION) {
        ShutdownAllVirtualMachines();
        quit = true;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int WINAPI _tWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PTSTR pCmdLine, int nCmdShow) {
    gflags::SetCommandLineOptionWithMode("v", "2", gflags::FlagSettingMode::SET_FLAGS_DEFAULT);
    gflags::SetCommandLineOptionWithMode("alsologtostderr", "true", gflags::FlagSettingMode::SET_FLAGS_DEFAULT);
    gflags::SetCommandLineOptionWithMode("logbuflevel", "-1", gflags::FlagSettingMode::SET_FLAGS_DEFAULT);

    auto args = ToArgv(__argc, __targv);
    int argc = __argc;
    char** argv = args.first.data();
    gflags::ParseCommandLineFlags(&argc, &argv, false);

    google::InitGoogleLogging(argv[0]);

    if (FLAGS_debug) {
        ShutdownAllVirtualMachines();
        return 0;
    }

    LPCTSTR ClassName = _T("VMWareAutoShutdown");

    WNDCLASSEX windowClass;
    ZeroMemory(&windowClass, sizeof(windowClass));
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &EventHandler;
    windowClass.hInstance = hInstance;
    windowClass.lpszClassName = ClassName;

    ATOM classId = RegisterClassEx(&windowClass);
    LOG_IF(FATAL, classId == 0) << "Failed to register class: " << GetLastErrorMessage();
    LOG(INFO) << "Class registered: 0x" << std::hex << classId;

    HWND windowHandle = CreateWindowEx(
        WS_EX_LEFT,
        ClassName,
        _T("VMWare Automatic Shutdown"),
        0,
        0,
        0,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        nullptr,
        nullptr,
        hInstance,
        nullptr);
    LOG_IF(FATAL, windowHandle == nullptr) << "Failed to create window: " << GetLastErrorMessage();
    LOG(INFO) << "Window created: 0x" << std::hex << windowHandle;

    while (!quit) {
        MSG msg;
        BOOL ret = GetMessage(&msg, windowHandle, 0, 0);
        if (ret != -1) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            LOG(ERROR) << "Failed to get message: " << GetLastErrorMessage();
        }
    }

    return 0;
}