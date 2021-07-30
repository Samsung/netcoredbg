// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <sstream>
#include <iterator>
#include <arrayholder.h>
#include "debugger/evalstackmachine.h"
#include "debugger/evaluator.h"
#include "debugger/evalhelpers.h"
#include "debugger/evalwaiter.h"
#include "debugger/valueprint.h"
#include "managed/interop.h"
#include "utils/utf.h"


namespace netcoredbg
{

namespace
{
    struct FormatF
    {
        uint32_t Flags;
    };

    struct FormatFS
    {
        uint32_t Flags;
        BSTR wString;
    };

    struct FormatFI
    {
        uint32_t Flags;
        int32_t Int;
    };

    struct FormatFIS
    {
        uint32_t Flags;
        int32_t Int;
        BSTR wString;
    };

    struct FormatFIP
    {
        uint32_t Flags;
        int32_t Int;
        PVOID Ptr;
    };

    HRESULT GetValueByParts(std::list<EvalStackEntry> &evalStack, const std::vector<EvaluationPart> &parts, ICorDebugValue **ppResultValue, std::string &output, EvalData &ed)
    {
        if (parts.empty())
            return E_INVALIDARG;

        HRESULT Status;
        if (FAILED(Status = ed.sharedEvaluator->EvalExpr(ed.pThread, ed.frameLevel, evalStack.front().parts, ppResultValue, ed.evalFlags)))
        {
            std::ostringstream ss;
            for (size_t i = 0; i < evalStack.front().parts.size(); i++)
            {
                if (evalStack.front().parts[i].indexes.empty())
                {
                    if (i != 0)
                        ss << ".";
                    ss << evalStack.front().parts[i].name;
                }
                else
                {
                    ss << "[";
                    for (size_t j = 0; j < evalStack.front().parts[i].indexes.size(); j++)
                    {
                        if (j != 0)
                            ss << ",";
                        ss << evalStack.front().parts[i].indexes[j];
                    }
                    ss << "]";
                }
            }
            output = "error: The name '" + ss.str() + "' does not exist in the current context";
            return Status;
        }

        return S_OK;
    }

    void ReplaceAllSubstring(std::string &str, const std::string &from, const std::string &to)
    {
        size_t start = 0;
        while (true)
        {
            start = str.find(from, start);
            if(start == std::string::npos)
                break;

            str.replace(start, from.length(), to);
            start += from.length();
        }
    }

    void ReplaceInternalNames(std::string &expression, bool restore = false)
    {
        // TODO more internal names should be added: $thread, ... (see internal variables supported by MSVS C# debugger)
        static std::vector<std::pair<std::string,std::string>> internalNamesMap {{"$exception", "__INTERNAL_NCDB_EXCEPTION_VARIABLE"}};

        for (auto &entry : internalNamesMap)
        {
            if (restore)
                ReplaceAllSubstring(expression, entry.second, entry.first);
            else
                ReplaceAllSubstring(expression, entry.first, entry.second);
        }
    }

    HRESULT CreatePrimitiveValue(ICorDebugThread *pThread, ICorDebugValue **ppValue, CorElementType type, PVOID ptr)
    {
        HRESULT Status;
        ToRelease<ICorDebugEval> iCorEval;
        IfFailRet(pThread->CreateEval(&iCorEval));
        IfFailRet(iCorEval->CreateValue(type, nullptr, ppValue));

        ToRelease<ICorDebugGenericValue> iCorGenValue;
        IfFailRet((*ppValue)->QueryInterface(IID_ICorDebugGenericValue, (LPVOID *) &iCorGenValue));
        return iCorGenValue->SetValue(ptr);
    }

    HRESULT CreateBooleanValue(ICorDebugThread *pThread, ICorDebugValue **ppValue, bool setToTrue)
    {
        HRESULT Status;
        ToRelease<ICorDebugEval> iCorEval;
        IfFailRet(pThread->CreateEval(&iCorEval));
        IfFailRet(iCorEval->CreateValue(ELEMENT_TYPE_BOOLEAN, nullptr, ppValue));

        if (!setToTrue)
            return S_OK;

        ULONG32 cbSize;
        IfFailRet((*ppValue)->GetSize(&cbSize));
        std::unique_ptr<BYTE[]> valueData(new (std::nothrow) BYTE[cbSize]);
        if (valueData == nullptr)
            return E_OUTOFMEMORY;
        memset(valueData.get(), 0, cbSize * sizeof(BYTE));

        ToRelease<ICorDebugGenericValue> iCorGenValue;
        IfFailRet((*ppValue)->QueryInterface(IID_ICorDebugGenericValue, (LPVOID *) &iCorGenValue));

        IfFailRet(iCorGenValue->GetValue((LPVOID) &(valueData[0])));
        valueData[0] = 1; // TRUE

        return iCorGenValue->SetValue((LPVOID) &(valueData[0]));
    }

    HRESULT CreateNullValue(ICorDebugThread *pThread, ICorDebugValue **ppValue)
    {
        HRESULT Status;
        ToRelease<ICorDebugEval> iCorEval;
        IfFailRet(pThread->CreateEval(&iCorEval));
        // ICorDebugEval::CreateValue
        // If the value of elementType is ELEMENT_TYPE_CLASS, you get an "ICorDebugReferenceValue" (returned in ppValue)
        // representing the null object reference. You can use this object to pass null to a function evaluation that has
        // object reference parameters. You cannot set the ICorDebugValue to anything; it always remains null.
        return iCorEval->CreateValue(ELEMENT_TYPE_CLASS, nullptr, ppValue);
    }

    HRESULT CreateDecimalValue(EvalWaiter *pEvalWaiter, ICorDebugThread *pThread, ICorDebugClass *pDecimalClass, ICorDebugValue **ppValue, PVOID ptr)
    {
        HRESULT Status;
        // Create value (without calling a constructor)
        IfFailRet(pEvalWaiter->WaitEvalResult(pThread, ppValue,
            [&](ICorDebugEval *pEval) -> HRESULT
            {
                // Note, this code execution protected by EvalWaiter mutex.
                ToRelease<ICorDebugEval2> pEval2;
                IfFailRet(pEval->QueryInterface(IID_ICorDebugEval2, (LPVOID*) &pEval2));
                IfFailRet(pEval2->NewParameterizedObjectNoConstructor(pDecimalClass, 0, nullptr));
                return S_OK;
            }));

        ToRelease<ICorDebugValue> pEditableValue;
        IfFailRet(DereferenceAndUnboxValue(*ppValue, &pEditableValue, nullptr));

        ToRelease<ICorDebugGenericValue> pGenericValue;
        IfFailRet(pEditableValue->QueryInterface(IID_ICorDebugGenericValue, (LPVOID*) &pGenericValue));
        return pGenericValue->SetValue(ptr);
    }

    HRESULT GetElementIndex(ICorDebugValue *pInputValue, ULONG32 &index)
    {
        HRESULT Status;

        BOOL isNull = TRUE;
        ToRelease<ICorDebugValue> pValue;
        IfFailRet(DereferenceAndUnboxValue(pInputValue, &pValue, &isNull));

        if(isNull)
            return E_INVALIDARG;

        ULONG32 cbSize;
        IfFailRet(pValue->GetSize(&cbSize));
        ArrayHolder<BYTE> indexValue = new (std::nothrow) BYTE[cbSize];
        if (indexValue == nullptr)
            return E_OUTOFMEMORY;

        memset(indexValue.GetPtr(), 0, cbSize * sizeof(BYTE));

        ToRelease<ICorDebugGenericValue> pGenericValue;
        IfFailRet(pValue->QueryInterface(IID_ICorDebugGenericValue, (LPVOID*) &pGenericValue));
        IfFailRet(pGenericValue->GetValue((LPVOID) &(indexValue[0])));

        CorElementType elemType;
        IfFailRet(pValue->GetType(&elemType));

        switch (elemType)
        {
            case ELEMENT_TYPE_I1:
            {
                int8_t tmp = *(int8_t*) &(indexValue[0]);
                if (tmp < 0)
                    return E_INVALIDARG;
                index = ULONG32((uint8_t)tmp);
                break;
            }
            case ELEMENT_TYPE_U1:
            {
                index = ULONG32(*(uint8_t*) &(indexValue[0]));
                break;
            }
            case ELEMENT_TYPE_I2:
            {
                int16_t tmp = *(int16_t*) &(indexValue[0]);
                if (tmp < 0)
                    return E_INVALIDARG;
                index = ULONG32((uint16_t)tmp);
                break;
            }
            case ELEMENT_TYPE_U2:
            {
                index = ULONG32(*(uint16_t*) &(indexValue[0]));
                break;
            }
            case ELEMENT_TYPE_I4:
            {
                int32_t tmp = *(int32_t*) &(indexValue[0]);
                if (tmp < 0)
                    return E_INVALIDARG;
                index = ULONG32(tmp);
                break;
            }
            case ELEMENT_TYPE_U4:
            {
                index = ULONG32(*(uint32_t*) &(indexValue[0]));
                break;
            }
            case ELEMENT_TYPE_I8:
            {
                int64_t tmp = *(int64_t*) &(indexValue[0]);
                if (tmp < 0)
                    return E_INVALIDARG;
                index = ULONG32(tmp);
                break;
            }
            case ELEMENT_TYPE_U8:
            {
                index = ULONG32(*(uint64_t*) &(indexValue[0]));
                break;
            }
            default:
                return E_INVALIDARG;
        }

        return S_OK;
    }


    HRESULT IdentifierName(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        std::string String = to_utf8(((FormatFS*)pArguments)->wString);
        ReplaceInternalNames(String, true);

        // TODO care about case with empty `parts` but available `iCorValue`, for example, in case of expression `a.b().c`
        evalStack.emplace_front();
        evalStack.front().parts.emplace_back(String);
        return S_OK;
    }

    HRESULT GenericName(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatFIS*)pArguments)->Flags;
        // TODO int32_t Int = ((FormatFIS*)pArguments)->Int;
        // TODO std::string String = to_utf8(((FormatFIS*)pArguments)->wString);
        return E_NOTIMPL;
    }

    HRESULT InvocationExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatFI*)pArguments)->Flags;
        // TODO int32_t Int = ((FormatFI*)pArguments)->Int;
        return E_NOTIMPL;
    }

    HRESULT ObjectCreationExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatFI*)pArguments)->Flags;
        // TODO int32_t Int = ((FormatFI*)pArguments)->Int;
        return E_NOTIMPL;
    }

    HRESULT ElementAccessExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        int32_t Int = ((FormatFI*)pArguments)->Int;

        HRESULT Status;
        std::vector<ULONG32> indexes;

        for (int32_t i = 0; i < Int; i++)
        {
            if (!evalStack.front().iCorValue)
                IfFailRet(GetValueByParts(evalStack,evalStack.front().parts, &evalStack.front().iCorValue, output, ed));

            // TODO implicitly convert iCorValue to int, if type not int
            //      at this moment GetElementIndex() work with integer types only

            ULONG32 result_index = 0;
            IfFailRet(GetElementIndex(evalStack.front().iCorValue, result_index));
            evalStack.pop_front();

            indexes.insert(indexes.begin(), result_index);
        }

        // TODO care about case with empty `parts` but available `iCorValue`, for example, in case of expression `a.b()[c]`
        evalStack.front().parts.emplace_back(indexes);
        return S_OK;
    }

    HRESULT ElementBindingExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT NumericLiteralExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        int32_t Int = ((FormatFIP*)pArguments)->Int;
        PVOID Ptr = ((FormatFIP*)pArguments)->Ptr;

        // StackMachine type to CorElementType map.
        static const CorElementType BasicTypesAlias[] {
            ELEMENT_TYPE_MAX, // Boolean - TrueLiteralExpression or FalseLiteralExpression
            ELEMENT_TYPE_MAX, // Byte - no literal suffix for byte
            ELEMENT_TYPE_MAX, // Char - CharacterLiteralExpression
            ELEMENT_TYPE_VALUETYPE, // Decimal
            ELEMENT_TYPE_R8,
            ELEMENT_TYPE_R4,
            ELEMENT_TYPE_I4,
            ELEMENT_TYPE_I8,
            ELEMENT_TYPE_MAX, // Object
            ELEMENT_TYPE_MAX, // SByte - no literal suffix for sbyte
            ELEMENT_TYPE_MAX, // Short - no literal suffix for short
            ELEMENT_TYPE_MAX, // String - StringLiteralExpression
            ELEMENT_TYPE_MAX, // UShort - no literal suffix for ushort
            ELEMENT_TYPE_U4,
            ELEMENT_TYPE_U8
        };

        evalStack.emplace_front();
        if (BasicTypesAlias[Int] == ELEMENT_TYPE_VALUETYPE)
            return CreateDecimalValue(ed.pEvalWaiter, ed.pThread, ed.pDecimalClass, &evalStack.front().iCorValue, Ptr);
        else
            return CreatePrimitiveValue(ed.pThread, &evalStack.front().iCorValue, BasicTypesAlias[Int], Ptr);
    }

    HRESULT StringLiteralExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        std::string String = to_utf8(((FormatFS*)pArguments)->wString);
        ReplaceInternalNames(String, true);
        evalStack.emplace_front();
        return ed.pEvalHelpers->CreateString(ed.pThread, String, &evalStack.front().iCorValue);
    }

    HRESULT CharacterLiteralExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        PVOID Ptr = ((FormatFIP*)pArguments)->Ptr;
        evalStack.emplace_front();
        return CreatePrimitiveValue(ed.pThread, &evalStack.front().iCorValue, ELEMENT_TYPE_CHAR, Ptr);
    }

    HRESULT PredefinedType(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatFI*)pArguments)->Flags;
        // TODO int32_t Int = ((FormatFI*)pArguments)->Int;
        return E_NOTIMPL;
    }

    HRESULT QualifiedName(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT AliasQualifiedName(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT MemberBindingExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT ConditionalExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT SimpleMemberAccessExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        assert(evalStack.size() > 1);

        std::vector<EvaluationPart> vector2 = std::move(evalStack.front().parts);
        evalStack.pop_front();
        std::vector<EvaluationPart> vector1 = std::move(evalStack.front().parts);
        evalStack.pop_front();

        // Note we create new entry here instead of old entry reusing in order to be sure we reset all parts of entry state (that could be added in future).
        evalStack.emplace_front();
        evalStack.front().parts = std::move(vector1);
        evalStack.front().parts.insert(evalStack.front().parts.end(), vector2.begin(), vector2.end());

        return S_OK;
    }

    HRESULT PointerMemberAccessExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT CastExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT AsExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT AddExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT MultiplyExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT SubtractExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT DivideExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT ModuloExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT LeftShiftExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT RightShiftExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT BitwiseAndExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT BitwiseOrExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT ExclusiveOrExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT LogicalAndExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT LogicalOrExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT EqualsExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT NotEqualsExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT GreaterThanExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT LessThanExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT GreaterThanOrEqualExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT LessThanOrEqualExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT IsExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT UnaryPlusExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT UnaryMinusExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT LogicalNotExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT BitwiseNotExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT TrueLiteralExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        evalStack.emplace_front();
        return CreateBooleanValue(ed.pThread, &evalStack.front().iCorValue, true);
    }

    HRESULT FalseLiteralExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        evalStack.emplace_front();
        return CreateBooleanValue(ed.pThread, &evalStack.front().iCorValue, false);
    }

    HRESULT NullLiteralExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        evalStack.emplace_front();
        return CreateNullValue(ed.pThread, &evalStack.front().iCorValue);
    }

    HRESULT PreIncrementExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT PostIncrementExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT PreDecrementExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT PostDecrementExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT SizeOfExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT TypeOfExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT CoalesceExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT ThisExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        evalStack.emplace_front();
        evalStack.front().parts.emplace_back("this");
        return S_OK;
    }

} // unnamed namespace

HRESULT EvalStackMachine::Run(ICorDebugThread *pThread, FrameLevel frameLevel, int evalFlags, const std::string &expression,
                              ICorDebugValue **ppResultValue, std::string &output)
{
    static const std::vector<std::function<HRESULT(std::list<EvalStackEntry>&, PVOID, std::string&, EvalData&)>> CommandImplementation = {
        IdentifierName,
        GenericName,
        InvocationExpression,
        ObjectCreationExpression,
        ElementAccessExpression,
        ElementBindingExpression,
        NumericLiteralExpression,
        StringLiteralExpression,
        CharacterLiteralExpression,
        PredefinedType,
        QualifiedName,
        AliasQualifiedName,
        MemberBindingExpression,
        ConditionalExpression,
        SimpleMemberAccessExpression,
        PointerMemberAccessExpression,
        CastExpression,
        AsExpression,
        AddExpression,
        MultiplyExpression,
        SubtractExpression,
        DivideExpression,
        ModuloExpression,
        LeftShiftExpression,
        RightShiftExpression,
        BitwiseAndExpression,
        BitwiseOrExpression,
        ExclusiveOrExpression,
        LogicalAndExpression,
        LogicalOrExpression,
        EqualsExpression,
        NotEqualsExpression,
        GreaterThanExpression,
        LessThanExpression,
        GreaterThanOrEqualExpression,
        LessThanOrEqualExpression,
        IsExpression,
        UnaryPlusExpression,
        UnaryMinusExpression,
        LogicalNotExpression,
        BitwiseNotExpression,
        TrueLiteralExpression,
        FalseLiteralExpression,
        NullLiteralExpression,
        PreIncrementExpression,
        PostIncrementExpression,
        PreDecrementExpression,
        PostDecrementExpression,
        SizeOfExpression,
        TypeOfExpression,
        CoalesceExpression,
        ThisExpression
    };

    // Note, internal variables start with "$" and must be replaced before CSharp syntax analyzer.
    // This data will be restored after CSharp syntax analyzer in IdentifierName and StringLiteralExpression.
    std::string fixed_expression = expression;
    ReplaceInternalNames(fixed_expression);

    HRESULT Status;
    PVOID pStackProgram = nullptr;
    if (FAILED(Status = Interop::GenerateStackMachineProgram(fixed_expression, &pStackProgram, output)) ||
        Status == S_FALSE) // return not error but S_FALSE in case some syntax kind not implemented.
    {
        return Status;
    }

    static constexpr int32_t ProgramFinished = -1;
    int32_t Command;
    PVOID pArguments;

    m_evalData.pThread = pThread;
    m_evalData.frameLevel = frameLevel;
    m_evalData.evalFlags = evalFlags;

    do
    {
        if (FAILED(Status = Interop::NextStackCommand(pStackProgram, Command, pArguments, output)) ||
            Command == ProgramFinished ||
            FAILED(Status = CommandImplementation[Command](m_evalStack, pArguments, output, m_evalData)))
            break;
    }
    while (1);

    if (SUCCEEDED(Status))
    {
        assert(m_evalStack.size() == 1);

        if (m_evalStack.front().iCorValue)
            *ppResultValue = m_evalStack.front().iCorValue.Detach();
        else
            Status = GetValueByParts(m_evalStack, m_evalStack.front().parts, ppResultValue, output, m_evalData);
    }

    Interop::ReleaseStackMachineProgram(pStackProgram);
    m_evalStack.clear();
    return Status;
}

HRESULT EvalStackMachine::FindPredefinedTypes(ICorDebugModule *pModule)
{
    HRESULT Status;
    ToRelease<IUnknown> pMDUnknown;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    mdTypeDef typeDef = mdTypeDefNil;
    static const WCHAR strTypeDef[] = W("System.Decimal");
    IfFailRet(pMD->FindTypeDefByName(strTypeDef, NULL, &typeDef));
    IfFailRet(pModule->GetClassFromToken(typeDef, &m_iCorDecimalClass));

    m_evalData.pDecimalClass = m_iCorDecimalClass.GetPtr();
    return S_OK;
}

} // namespace netcoredbg
