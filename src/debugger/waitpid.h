// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#ifdef FEATURE_PAL

#include <signal.h>
#include <mutex>

namespace netcoredbg
{
namespace hook
{

class waitpid_t
{
private:
    typedef pid_t (*Signature)(pid_t pid, int *status, int options);
    Signature original = nullptr;
    static constexpr pid_t notConfigured = -1;
    pid_t trackPID = notConfigured;
    int exitCode = 0; // same behaviour as CoreCLR have, by default exit code is 0
    std::recursive_mutex interlock;

#ifdef INTEROP_DEBUGGING
    std::mutex pidMutex;
    bool interopWaitpidMode = false;
    bool pidExited = false;
    int pidStatus = 0;
    pid_t pidPid = 0;
#endif // INTEROP_DEBUGGING

    waitpid_t(const waitpid_t&) = delete;
    waitpid_t& operator=(const waitpid_t&) = delete;

    void init() noexcept;

public:
    waitpid_t() = default;
    ~waitpid_t() = default;

    pid_t operator() (pid_t pid, int *status, int options);
    void SetupTrackingPID(pid_t PID);
    int GetExitCode();
    void SetExitCode(pid_t PID, int Code);

#ifdef INTEROP_DEBUGGING
    void SetInteropWaitpidMode(bool mode);
    bool IsInteropWaitpidMode();
    void InitPidStatus(pid_t pid);
    void SetPidExitedStatus(pid_t pid, int status);
    bool GetPidExitedStatus(pid_t &pid, int &status);
#endif // INTEROP_DEBUGGING
};

} // namespace hook

hook::waitpid_t &GetWaitpid();

} // namespace netcoredbg

#endif // FEATURE_PAL
