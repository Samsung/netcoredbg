// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

HRESULT CreateVar(ICorDebugThread *pThread, ICorDebugFrame *pFrame, const std::string &varobjName, const std::string &expression, std::string &output);
HRESULT ListChildren(int childStart, int childEnd, const std::string &name, int print_values, ICorDebugThread *pThread, ICorDebugFrame *pFrame, std::string &output);
HRESULT DeleteVar(const std::string &varobjName);
void CleanupVars();

HRESULT RunClassConstructor(ICorDebugThread *pThread, ICorDebugValue *pValue);
