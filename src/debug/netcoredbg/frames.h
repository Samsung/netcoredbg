// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.
#pragma once

#include <vector>

#include <cor.h>
#include <cordebug.h>

struct Thread;

HRESULT GetFrameAt(ICorDebugThread *pThread, int level, ICorDebugFrame **ppFrame);
HRESULT GetThreadsState(ICorDebugController *controller, std::vector<Thread> &threads);
