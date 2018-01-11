// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

HRESULT DeleteBreakpoint(ULONG32 id);
HRESULT InsertBreakpointInProcess(ICorDebugProcess *pProcess, std::string filename, int linenum, Breakpoint &breakpoint);
void InsertExceptionBreakpoint(const std::string &name, Breakpoint &breakpoint);
HRESULT GetCurrentBreakpoint(ICorDebugThread *pThread, Breakpoint &breakpoint);

void DeleteAllBreakpoints();
HRESULT HitBreakpoint(ICorDebugThread *pThread, Breakpoint &breakpoint);
void TryResolveBreakpointsForModule(ICorDebugModule *pModule);
