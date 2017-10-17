// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

HRESULT GetType(const std::string &typeName, ICorDebugThread *pThread, ICorDebugType **ppType);
HRESULT EvalExpr(ICorDebugThread *pThread, ICorDebugFrame *pFrame, const std::string &expression, ICorDebugValue **ppResult);
