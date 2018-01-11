// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

HRESULT GetFrameLocation(ICorDebugFrame *pFrame, StackFrame &stackFrame);
HRESULT GetFrameAt(ICorDebugThread *pThread, int level, ICorDebugFrame **ppFrame);
HRESULT GetThreadsState(ICorDebugController *controller, std::vector<Thread> &threads);
HRESULT GetStackTrace(ICorDebugThread *pThread, int lowFrame, int highFrame, std::vector<StackFrame> &stackFrames);
