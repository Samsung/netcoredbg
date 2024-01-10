// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/sigaction.h"

#ifdef FEATURE_PAL
#include <dlfcn.h>
#include <stdlib.h>
#include "utils/logger.h"

namespace netcoredbg
{
namespace hook
{

void sigaction_t::init() noexcept
{
    auto ret = dlsym(RTLD_NEXT, "sigaction");
    if (!ret)
    {
        LOGE("Could not find original function sigaction");
        abort();
    }
    original = reinterpret_cast<Signature>(ret);
}

int sigaction_t::operator() (int signum, const struct sigaction *act, struct sigaction *oldact)
{
    std::lock_guard<std::recursive_mutex> mutex_guard(interlock);
    if (!original)
    {
        init();
    }
    return original(signum, act, oldact);
}

sigaction_t g_sigaction;

} // namespace hook

void SetSigactionMode(bool interopDebugging)
{
    hook::g_sigaction.interopDebuggingMode = interopDebugging;
    if (interopDebugging)
    {
        struct sigaction sa;
        // workaround for `warning: nested designators are a C99 extension [-Wc99-designator]`
        sa.sa_handler = SIG_DFL;
        const struct sigaction &c_sa = sa;
        if (hook::g_sigaction(SIGCHLD, &c_sa, NULL) == -1)
            LOGE("Failed SIGCHLD sigaction setup to SIG_DFL\n");
    }
}

hook::sigaction_t &GetSigaction()
{
    return hook::g_sigaction;
}

extern "C" int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
    if (signum == SIGCHLD && hook::g_sigaction.interopDebuggingMode)
    {
        LOGW("sigaction for SIGCHLD with interop debugging are prohibited");
        // `sigaction() returns 0 on success`, make sure initial caller (our managed part) think all is OK.
        return 0;
    }

    return hook::g_sigaction(signum, act, oldact);
}

} // namespace netcoredbg

#endif // FEATURE_PAL
