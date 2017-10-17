// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

HRESULT PrintFrameLocation(ICorDebugFrame *pFrame, std::string &output);
HRESULT GetFrameAt(ICorDebugThread *pThread, int level, ICorDebugFrame **ppFrame);

HRESULT PrintThreadsState(ICorDebugController *controller, std::string &output);
HRESULT PrintFrames(ICorDebugThread *pThread, std::string &output, int lowFrame = 0, int highFrame = INT_MAX);
