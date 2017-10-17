// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

HRESULT DeleteBreakpoint(ULONG32 id);
HRESULT InsertBreakpointInProcess(ICorDebugProcess *pProcess, std::string filename, int linenum, ULONG32 &id);
HRESULT PrintBreakpoint(ULONG32 id, std::string &output);
ULONG32 InsertExceptionBreakpoint(const std::string &name);
HRESULT CreateBreakpointInProcess(ICorDebugProcess *pProcess, std::string filename, int linenum, ULONG32 &id);
HRESULT PrintBreakpoint(ULONG32 id, std::string &output);

void DeleteAllBreakpoints();
HRESULT HitBreakpoint(ICorDebugThread *pThread, ULONG32 &id, ULONG32 &times);
void TryResolveBreakpointsForModule(ICorDebugModule *pModule);
