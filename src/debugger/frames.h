// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include "cor.h"
#include "cordebug.h"

#include <vector>
#include "protocols/protocol.h"

namespace netcoredbg
{

enum FrameType
{
    FrameUnknown,
    FrameNative,
    FrameCLRNative,
    FrameCLRInternal,
    FrameCLRManaged
};

struct NativeFrame
{
    uint64_t addr;
    std::string symbol;
    std::string file;
    std::string fullname;
    int linenum;
    int tid;
    NativeFrame() : addr(0), linenum(0), tid(0) {}
};

typedef std::function<HRESULT(FrameType,ICorDebugFrame*,NativeFrame*,ICorDebugFunction*)> WalkFramesCallback;

struct Thread;

HRESULT GetFrameAt(ICorDebugThread *pThread, FrameLevel level, ICorDebugFrame **ppFrame);
HRESULT GetThreadsState(ICorDebugController *controller, std::vector<Thread> &threads);
uint64_t GetFrameAddr(ICorDebugFrame *pFrame);
const char *GetInternalTypeName(CorDebugInternalFrameType frameType);
HRESULT WalkFrames(ICorDebugThread *pThread, WalkFramesCallback cb);

} // namespace netcoredbg
