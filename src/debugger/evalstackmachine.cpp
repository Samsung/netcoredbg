// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <array>
#include <functional>
#include <sstream>
#include <iterator>
#include <arrayholder.h>
#include "debugger/evalstackmachine.h"
#include "debugger/evalhelpers.h"
#include "debugger/evalwaiter.h"
#include "debugger/valueprint.h"
#include "managed/interop.h"
#include "metadata/typeprinter.h"
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

    // Keep in sync with BasicTypes enum in Evaluation.cs
    enum class BasicTypes : int32_t
    {
        TypeBoolean = 1,
        TypeByte,
        TypeSByte,
        TypeChar,
        TypeDouble,
        TypeSingle,
        TypeInt32,
        TypeUInt32,
        TypeInt64,
        TypeUInt64,
        TypeInt16,
        TypeUInt16,
        TypeString
    };

    // Keep in sync with OperationType enum in Evaluation.cs
    enum class OperationType : int32_t
    {
        AddExpression = 1,
        SubtractExpression,
        MultiplyExpression,
        DivideExpression,
        ModuloExpression,
        RightShiftExpression,
        LeftShiftExpression,
        BitwiseNotExpression,
        LogicalAndExpression,
        LogicalOrExpression,
        ExclusiveOrExpression,
        BitwiseAndExpression,
        BitwiseOrExpression,
        LogicalNotExpression,
        EqualsExpression,
        NotEqualsExpression,
        LessThanExpression,
        GreaterThanExpression,
        LessThanOrEqualExpression,
        GreaterThanOrEqualExpression,
        UnaryPlusExpression,
        UnaryMinusExpression
    };

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

        if (!ptr)
            return S_OK;

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

    HRESULT CreateValueType(EvalWaiter *pEvalWaiter, ICorDebugThread *pThread, ICorDebugClass *pValueTypeClass, ICorDebugValue **ppValue, PVOID ptr)
    {
        HRESULT Status;
        // Create value (without calling a constructor)
        IfFailRet(pEvalWaiter->WaitEvalResult(pThread, ppValue,
            [&](ICorDebugEval *pEval) -> HRESULT
            {
                // Note, this code execution protected by EvalWaiter mutex.
                ToRelease<ICorDebugEval2> pEval2;
                IfFailRet(pEval->QueryInterface(IID_ICorDebugEval2, (LPVOID*) &pEval2));
                IfFailRet(pEval2->NewParameterizedObjectNoConstructor(pValueTypeClass, 0, nullptr));
                return S_OK;
            }));

        if (!ptr)
            return S_OK;

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

    HRESULT GetFrontStackEntryValue(ICorDebugValue **ppResultValue, std::unique_ptr<Evaluator::SetterData> *resultSetterData, std::list<EvalStackEntry> &evalStack, EvalData &ed, std::string &output)
    {
        HRESULT Status;
        Evaluator::SetterData *inputPropertyData = nullptr;
        if (evalStack.front().editable)
            inputPropertyData = evalStack.front().setterData.get();
        else
            resultSetterData = nullptr;

        if (FAILED(Status = ed.pEvaluator->ResolveIdentifiers(ed.pThread, ed.frameLevel, evalStack.front().iCorValue, inputPropertyData,  evalStack.front().identifiers,
                                                              ppResultValue, resultSetterData, nullptr, ed.evalFlags))
            && !evalStack.front().identifiers.empty())
        {
            std::ostringstream ss;
            for (size_t i = 0; i < evalStack.front().identifiers.size(); i++)
            {
                if (i != 0)
                    ss << ".";
                ss << evalStack.front().identifiers[i];
            }
            output = "error: The name '" + ss.str() + "' does not exist in the current context";
        }

        return Status;
    }

    HRESULT GetFrontStackEntryType(ICorDebugType **ppResultType, std::list<EvalStackEntry> &evalStack, EvalData &ed, std::string &output)
    {
        HRESULT Status;
        ToRelease<ICorDebugValue> iCorValue;
        if ((FAILED(Status = ed.pEvaluator->ResolveIdentifiers(ed.pThread, ed.frameLevel, evalStack.front().iCorValue, nullptr, evalStack.front().identifiers,
                                                               &iCorValue, nullptr, ppResultType, ed.evalFlags))
            && !evalStack.front().identifiers.empty()) || iCorValue)
        {
            std::ostringstream ss;
            for (size_t i = 0; i < evalStack.front().identifiers.size(); i++)
            {
                if (i != 0)
                    ss << ".";
                ss << evalStack.front().identifiers[i];
            }
            if(!iCorValue)
                output = "error: The type or namespace name '" + ss.str() + "' couldn't be found";
            else
                output = "error: '" + ss.str() + "' is a variable but is used like a type";
            if (SUCCEEDED(Status))
                Status = E_FAIL;
        }

        return Status;
    }

    HRESULT GetIndexesFromStack(std::vector<ULONG32> &indexes, int dimension, std::list<EvalStackEntry> &evalStack, EvalData &ed, std::string &output)
    {
        HRESULT Status;

        for (int32_t i = 0; i < dimension; i++)
        {
            ToRelease<ICorDebugValue> iCorValue;
            IfFailRet(GetFrontStackEntryValue(&iCorValue, nullptr, evalStack, ed, output));
            evalStack.pop_front();

            // TODO implicitly convert iCorValue to int, if type not int
            //      at this moment GetElementIndex() work with integer types only

            ULONG32 result_index = 0;
            IfFailRet(GetElementIndex(iCorValue, result_index));
            indexes.insert(indexes.begin(), result_index);
        }

        return S_OK;
    }

    HRESULT GetArgData(ICorDebugValue *pTypeValue, std::string &typeName, CorElementType &elemType)
    {
        HRESULT Status;
        IfFailRet(pTypeValue->GetType(&elemType));
        if (elemType == ELEMENT_TYPE_CLASS || elemType == ELEMENT_TYPE_VALUETYPE)
        {
            ToRelease<ICorDebugValue2> iCorTypeValue2;
            IfFailRet(pTypeValue->QueryInterface(IID_ICorDebugValue2, (LPVOID *) &iCorTypeValue2));
            ToRelease<ICorDebugType> iCorType;
            IfFailRet(iCorTypeValue2->GetExactType(&iCorType));
            IfFailRet(TypePrinter::NameForTypeByType(iCorType, typeName));
        }
        return S_OK;
    };

    HRESULT CallUnaryOperator(const std::string &opName, ICorDebugValue *pValue, ICorDebugValue **pResultValue, EvalData &ed)
    {
        HRESULT Status;
        std::string typeName;
        CorElementType elemType;
        IfFailRet(GetArgData(pValue, typeName, elemType));

        ToRelease<ICorDebugFunction> iCorFunc;
        ed.pEvaluator->WalkMethods(pValue, [&](
            bool is_static,
            const std::string &methodName,
            Evaluator::ReturnElementType&,
            std::vector<Evaluator::ArgElementType> &methodArgs,
            Evaluator::GetFunctionCallback getFunction)
        {
            if (!is_static || methodArgs.size() != 1 || opName != methodName ||
                elemType != methodArgs[0].corType || typeName != methodArgs[0].typeName)
                return S_OK;

            IfFailRet(getFunction(&iCorFunc));

            return E_ABORT; // Fast exit from cycle.
        });
        if (!iCorFunc)
            return E_FAIL;

        return ed.pEvalHelpers->EvalFunction(ed.pThread, iCorFunc, nullptr, 0, &pValue, 1, pResultValue, ed.evalFlags);
    }

    HRESULT CallCastOperator(const std::string &opName, ICorDebugValue *pValue, CorElementType elemRetType, const std::string &typeRetName,
                             ICorDebugValue *pTypeValue, ICorDebugValue **pResultValue, EvalData &ed)
    {
        HRESULT Status;
        std::string typeName;
        CorElementType elemType;
        IfFailRet(GetArgData(pTypeValue, typeName, elemType));

        ToRelease<ICorDebugFunction> iCorFunc;
        ed.pEvaluator->WalkMethods(pValue, [&](
            bool is_static,
            const std::string &methodName,
            Evaluator::ReturnElementType& methodRet,
            std::vector<Evaluator::ArgElementType> &methodArgs,
            Evaluator::GetFunctionCallback getFunction)
        {
            if (!is_static || methodArgs.size() != 1 || opName != methodName ||
                elemRetType != methodRet.corType || typeRetName != methodRet.typeName ||
                elemType != methodArgs[0].corType || typeName != methodArgs[0].typeName)
                return S_OK;

            IfFailRet(getFunction(&iCorFunc));

            return E_ABORT; // Fast exit from cycle.
        });
        if (!iCorFunc)
            return E_FAIL;

        return ed.pEvalHelpers->EvalFunction(ed.pThread, iCorFunc, nullptr, 0, &pTypeValue, 1, pResultValue, ed.evalFlags);
    }

    HRESULT CallCastOperator(const std::string &opName, ICorDebugValue *pValue, ICorDebugValue *pTypeRetValue, ICorDebugValue *pTypeValue,
                             ICorDebugValue **pResultValue, EvalData &ed)
    {
        HRESULT Status;
        std::string typeRetName;
        CorElementType elemRetType;
        IfFailRet(GetArgData(pTypeRetValue, typeRetName, elemRetType));

        return CallCastOperator(opName, pValue, elemRetType, typeRetName, pTypeValue, pResultValue, ed);
    }

    template<typename T1, typename T2>
    HRESULT ImplicitCastElemType(ICorDebugValue *pValue1, ICorDebugValue *pValue2, bool testRange)
    {
        HRESULT Status;
        ToRelease<ICorDebugGenericValue> pGenericValue1;
        IfFailRet(pValue1->QueryInterface(IID_ICorDebugGenericValue, (LPVOID*) &pGenericValue1));
        T1 value1 = 0;
        IfFailRet(pGenericValue1->GetValue(&value1));

        if (testRange &&
            ((value1 < 0 && (std::numeric_limits<T2>::min() == 0 || value1 - std::numeric_limits<T2>::min() < 0)) ||
             (value1 > 0 && std::numeric_limits<T2>::max() - value1 < 0)))
            return E_INVALIDARG;

        ToRelease<ICorDebugGenericValue> pGenericValue2;
        IfFailRet(pValue2->QueryInterface(IID_ICorDebugGenericValue, (LPVOID*) &pGenericValue2));
        T2 value2 = (T2)value1;
        return pGenericValue2->SetValue(&value2);
    }

    typedef std::array< std::array<std::function<HRESULT(ICorDebugValue*,ICorDebugValue*,bool)>, ELEMENT_TYPE_MAX>, ELEMENT_TYPE_MAX> ImplicitCastMap_t;

    ImplicitCastMap_t InitImplicitCastMap()
    {
        ImplicitCastMap_t implicitCastMap;
        implicitCastMap[ELEMENT_TYPE_CHAR][ELEMENT_TYPE_U2] = ImplicitCastElemType<uint16_t, uint16_t>;
        implicitCastMap[ELEMENT_TYPE_CHAR][ELEMENT_TYPE_I4] = ImplicitCastElemType<uint16_t, int32_t>;
        implicitCastMap[ELEMENT_TYPE_CHAR][ELEMENT_TYPE_U4] = ImplicitCastElemType<uint16_t, uint32_t>;
        implicitCastMap[ELEMENT_TYPE_CHAR][ELEMENT_TYPE_I8] = ImplicitCastElemType<uint16_t, int64_t>;
        implicitCastMap[ELEMENT_TYPE_CHAR][ELEMENT_TYPE_U8] = ImplicitCastElemType<uint16_t, uint64_t>;
        implicitCastMap[ELEMENT_TYPE_CHAR][ELEMENT_TYPE_R4] = ImplicitCastElemType<uint16_t, float>;
        implicitCastMap[ELEMENT_TYPE_CHAR][ELEMENT_TYPE_R8] = ImplicitCastElemType<uint16_t, double>;
        implicitCastMap[ELEMENT_TYPE_I1][ELEMENT_TYPE_I2] = ImplicitCastElemType<int8_t, int16_t>;
        implicitCastMap[ELEMENT_TYPE_I1][ELEMENT_TYPE_I4] = ImplicitCastElemType<int8_t, int32_t>;
        implicitCastMap[ELEMENT_TYPE_I1][ELEMENT_TYPE_I8] = ImplicitCastElemType<int8_t, int64_t>;
        implicitCastMap[ELEMENT_TYPE_I1][ELEMENT_TYPE_R4] = ImplicitCastElemType<int8_t, float>;
        implicitCastMap[ELEMENT_TYPE_I1][ELEMENT_TYPE_R8] = ImplicitCastElemType<int8_t, double>;
        implicitCastMap[ELEMENT_TYPE_U1][ELEMENT_TYPE_I2] = ImplicitCastElemType<uint8_t, int16_t>;
        implicitCastMap[ELEMENT_TYPE_U1][ELEMENT_TYPE_U2] = ImplicitCastElemType<uint8_t, uint16_t>;
        implicitCastMap[ELEMENT_TYPE_U1][ELEMENT_TYPE_I4] = ImplicitCastElemType<uint8_t, int32_t>;
        implicitCastMap[ELEMENT_TYPE_U1][ELEMENT_TYPE_U4] = ImplicitCastElemType<uint8_t, uint32_t>;
        implicitCastMap[ELEMENT_TYPE_U1][ELEMENT_TYPE_I8] = ImplicitCastElemType<uint8_t, int64_t>;
        implicitCastMap[ELEMENT_TYPE_U1][ELEMENT_TYPE_U8] = ImplicitCastElemType<uint8_t, uint64_t>;
        implicitCastMap[ELEMENT_TYPE_U1][ELEMENT_TYPE_R4] = ImplicitCastElemType<uint8_t, float>;
        implicitCastMap[ELEMENT_TYPE_U1][ELEMENT_TYPE_R8] = ImplicitCastElemType<uint8_t, double>;
        implicitCastMap[ELEMENT_TYPE_I2][ELEMENT_TYPE_I4] = ImplicitCastElemType<int16_t, int32_t>;
        implicitCastMap[ELEMENT_TYPE_I2][ELEMENT_TYPE_I8] = ImplicitCastElemType<int16_t, int64_t>;
        implicitCastMap[ELEMENT_TYPE_I2][ELEMENT_TYPE_R4] = ImplicitCastElemType<int16_t, float>;
        implicitCastMap[ELEMENT_TYPE_I2][ELEMENT_TYPE_R8] = ImplicitCastElemType<int16_t, double>;
        implicitCastMap[ELEMENT_TYPE_U2][ELEMENT_TYPE_I4] = ImplicitCastElemType<uint16_t, int32_t>;
        implicitCastMap[ELEMENT_TYPE_U2][ELEMENT_TYPE_U4] = ImplicitCastElemType<uint16_t, uint32_t>;
        implicitCastMap[ELEMENT_TYPE_U2][ELEMENT_TYPE_I8] = ImplicitCastElemType<uint16_t, int64_t>;
        implicitCastMap[ELEMENT_TYPE_U2][ELEMENT_TYPE_U8] = ImplicitCastElemType<uint16_t, uint64_t>;
        implicitCastMap[ELEMENT_TYPE_U2][ELEMENT_TYPE_R4] = ImplicitCastElemType<uint16_t, float>;
        implicitCastMap[ELEMENT_TYPE_U2][ELEMENT_TYPE_R8] = ImplicitCastElemType<uint16_t, double>;
        implicitCastMap[ELEMENT_TYPE_I4][ELEMENT_TYPE_I8] = ImplicitCastElemType<int32_t, int64_t>;
        implicitCastMap[ELEMENT_TYPE_I4][ELEMENT_TYPE_R4] = ImplicitCastElemType<int32_t, float>;
        implicitCastMap[ELEMENT_TYPE_I4][ELEMENT_TYPE_R8] = ImplicitCastElemType<int32_t, double>;
        implicitCastMap[ELEMENT_TYPE_U4][ELEMENT_TYPE_I8] = ImplicitCastElemType<uint32_t, int64_t>;
        implicitCastMap[ELEMENT_TYPE_U4][ELEMENT_TYPE_U8] = ImplicitCastElemType<uint32_t, uint64_t>;
        implicitCastMap[ELEMENT_TYPE_U4][ELEMENT_TYPE_R4] = ImplicitCastElemType<uint32_t, float>;
        implicitCastMap[ELEMENT_TYPE_U4][ELEMENT_TYPE_R8] = ImplicitCastElemType<uint32_t, double>;
        implicitCastMap[ELEMENT_TYPE_I8][ELEMENT_TYPE_R4] = ImplicitCastElemType<int64_t, float>;
        implicitCastMap[ELEMENT_TYPE_I8][ELEMENT_TYPE_R8] = ImplicitCastElemType<int64_t, double>;
        implicitCastMap[ELEMENT_TYPE_U8][ELEMENT_TYPE_R4] = ImplicitCastElemType<uint64_t, float>;
        implicitCastMap[ELEMENT_TYPE_U8][ELEMENT_TYPE_R8] = ImplicitCastElemType<uint64_t, double>;
        implicitCastMap[ELEMENT_TYPE_R4][ELEMENT_TYPE_R8] = ImplicitCastElemType<float, double>;

        return implicitCastMap;
    }

    ImplicitCastMap_t InitImplicitCastLiteralMap()
    {
        ImplicitCastMap_t implicitCastLiteralMap;
        implicitCastLiteralMap[ELEMENT_TYPE_I4][ELEMENT_TYPE_I1] = ImplicitCastElemType<int32_t, int8_t>;
        implicitCastLiteralMap[ELEMENT_TYPE_I4][ELEMENT_TYPE_U1] = ImplicitCastElemType<int32_t, uint8_t>;
        implicitCastLiteralMap[ELEMENT_TYPE_I4][ELEMENT_TYPE_I2] = ImplicitCastElemType<int32_t, int16_t>;
        implicitCastLiteralMap[ELEMENT_TYPE_I4][ELEMENT_TYPE_U2] = ImplicitCastElemType<int32_t, uint16_t>;
        implicitCastLiteralMap[ELEMENT_TYPE_I4][ELEMENT_TYPE_U4] = ImplicitCastElemType<int32_t, uint32_t>;
        implicitCastLiteralMap[ELEMENT_TYPE_I4][ELEMENT_TYPE_U8] = ImplicitCastElemType<int32_t, uint64_t>;

        return implicitCastLiteralMap;
    }

    HRESULT GetRealValueWithType(ICorDebugValue *pValue, ICorDebugValue **ppResultValue, CorElementType *pElemType = nullptr)
    {
        HRESULT Status;
        // Dereference and unbox value, since we need real value.
        ToRelease<ICorDebugValue> iCorRealValue;
        IfFailRet(DereferenceAndUnboxValue(pValue, &iCorRealValue));
        CorElementType elemType;
        IfFailRet(iCorRealValue->GetType(&elemType));
        // Note, in case of class (string is class), we must use reference instead.
        if (elemType == ELEMENT_TYPE_STRING ||
            elemType == ELEMENT_TYPE_CLASS)
        {
            pValue->AddRef();
            *ppResultValue = pValue;
            if (pElemType)
                *pElemType = elemType;
        }
        else
        {
            *ppResultValue = iCorRealValue.Detach();
            if (pElemType)
                *pElemType = elemType;
        }

        return S_OK;
    }

    HRESULT CopyValue(ICorDebugValue *pSrcValue, ICorDebugValue *pDstValue, CorElementType elemTypeSrc, CorElementType elemTypeDst)
    {
        if (elemTypeSrc != elemTypeDst)
            return E_INVALIDARG;

        HRESULT Status;
        // Change address.
        if (elemTypeDst == ELEMENT_TYPE_STRING ||
            elemTypeDst == ELEMENT_TYPE_CLASS)
        {
            ToRelease<ICorDebugReferenceValue> pRefNew;
            IfFailRet(pSrcValue->QueryInterface(IID_ICorDebugReferenceValue, (LPVOID *) &pRefNew));
            ToRelease<ICorDebugReferenceValue> pRefOld;
            IfFailRet(pDstValue->QueryInterface(IID_ICorDebugReferenceValue, (LPVOID *) &pRefOld));

            CORDB_ADDRESS addr;
            IfFailRet(pRefNew->GetValue(&addr));
            return pRefOld->SetValue(addr);
        }

        // Copy data.
        if (elemTypeDst == ELEMENT_TYPE_BOOLEAN ||
            elemTypeDst == ELEMENT_TYPE_CHAR ||
            elemTypeDst == ELEMENT_TYPE_I1 ||
            elemTypeDst == ELEMENT_TYPE_U1 ||
            elemTypeDst == ELEMENT_TYPE_I2 ||
            elemTypeDst == ELEMENT_TYPE_U2 ||
            elemTypeDst == ELEMENT_TYPE_U4 ||
            elemTypeDst == ELEMENT_TYPE_I4 ||
            elemTypeDst == ELEMENT_TYPE_I8 ||
            elemTypeDst == ELEMENT_TYPE_U8 ||
            elemTypeDst == ELEMENT_TYPE_R4 ||
            elemTypeDst == ELEMENT_TYPE_R8 ||
            elemTypeDst == ELEMENT_TYPE_VALUETYPE)
        {
            ULONG32 cbSize;
            IfFailRet(pSrcValue->GetSize(&cbSize));
            ArrayHolder<BYTE> elemValue = new (std::nothrow) BYTE[cbSize];
            if (elemValue == nullptr)
                return E_OUTOFMEMORY;

            memset(elemValue.GetPtr(), 0, cbSize * sizeof(BYTE));

            ToRelease<ICorDebugGenericValue> pGenericValue;
            IfFailRet(pSrcValue->QueryInterface(IID_ICorDebugGenericValue, (LPVOID*) &pGenericValue));
            IfFailRet(pGenericValue->GetValue((LPVOID) &(elemValue[0])));

            pGenericValue.Free();
            IfFailRet(pDstValue->QueryInterface(IID_ICorDebugGenericValue, (LPVOID*) &pGenericValue));
            return pGenericValue->SetValue(elemValue.GetPtr());
        }

        return E_NOTIMPL;
    }

    HRESULT ImplicitCast(ICorDebugValue *pSrcValue, ICorDebugValue *pDstValue, bool srcLiteral, EvalData &ed)
    {
        HRESULT Status;

        // Value with type was provided by caller, result must be implicitly cast to this type.
        // https://docs.microsoft.com/en-us/dotnet/csharp/language-reference/language-specification/conversions#implicit-numeric-conversions
        // https://docs.microsoft.com/en-us/dotnet/csharp/language-reference/builtin-types/integral-numeric-types#integer-literals
        
        ToRelease<ICorDebugValue> iCorRealValue1;
        CorElementType elemType1;
        IfFailRet(GetRealValueWithType(pSrcValue, &iCorRealValue1, &elemType1));

        ToRelease<ICorDebugValue> iCorRealValue2;
        CorElementType elemType2;
        IfFailRet(GetRealValueWithType(pDstValue, &iCorRealValue2, &elemType2));

        bool haveSameType = true;
        if (elemType1 == elemType2)
        {
            if (elemType2 == ELEMENT_TYPE_VALUETYPE || elemType2 == ELEMENT_TYPE_CLASS)
            {
                std::string mdName1;
                IfFailRet(TypePrinter::NameForTypeByValue(iCorRealValue1, mdName1));
                std::string mdName2;
                IfFailRet(TypePrinter::NameForTypeByValue(iCorRealValue2, mdName2));

                if (mdName1 != mdName2)
                    haveSameType = false;
            }
        }
        else
        {
            haveSameType = false;
        }

        if (!haveSameType &&
            (elemType1 == ELEMENT_TYPE_VALUETYPE || elemType2 == ELEMENT_TYPE_VALUETYPE ||
             elemType1 == ELEMENT_TYPE_CLASS || elemType2 == ELEMENT_TYPE_CLASS))
        {
            ToRelease<ICorDebugValue> iCorResultValue;
            if (FAILED(Status = CallCastOperator("op_Implicit", iCorRealValue1, iCorRealValue2, iCorRealValue1, &iCorResultValue, ed)) &&
                FAILED(Status = CallCastOperator("op_Implicit", iCorRealValue2, iCorRealValue2, iCorRealValue1, &iCorResultValue, ed)))
                return Status;

            iCorRealValue1.Free();
            IfFailRet(GetRealValueWithType(iCorResultValue, &iCorRealValue1, &elemType1));

            haveSameType = true;
        }

        if (haveSameType)
            return CopyValue(iCorRealValue1, iCorRealValue2, elemType1, elemType2);

        static ImplicitCastMap_t implicitCastMap = InitImplicitCastMap();
        static ImplicitCastMap_t implicitCastLiteralMap = InitImplicitCastLiteralMap();

        if (srcLiteral && implicitCastLiteralMap[elemType1][elemType2] != nullptr)
            return implicitCastLiteralMap[elemType1][elemType2](iCorRealValue1, iCorRealValue2, true);

        if (implicitCastMap[elemType1][elemType2] != nullptr)
            return implicitCastMap[elemType1][elemType2](iCorRealValue1, iCorRealValue2, false);

        return E_INVALIDARG;
    }

    HRESULT GetOperandDataTypeByValue(ICorDebugValue *pValue, CorElementType elemType, PVOID &resultData, int32_t &resultType)
    {
        HRESULT Status;

        if (elemType == ELEMENT_TYPE_STRING)
        {
            resultType = (int32_t)BasicTypes::TypeString;
            ToRelease<ICorDebugValue> iCorValue;
            BOOL isNull = FALSE;
            IfFailRet(DereferenceAndUnboxValue(pValue, &iCorValue, &isNull));
            resultData = 0;
            if (!isNull)
            {
                std::string String;
                IfFailRet(PrintStringValue(iCorValue, String));
                resultData = Interop::AllocString(String);
            }
            return S_OK;
        }

        static std::unordered_map<CorElementType, BasicTypes> basicTypesMap
        {
            {ELEMENT_TYPE_BOOLEAN, BasicTypes::TypeBoolean},
            {ELEMENT_TYPE_U1, BasicTypes::TypeByte},
            {ELEMENT_TYPE_I1, BasicTypes::TypeSByte},
            {ELEMENT_TYPE_CHAR, BasicTypes::TypeChar},
            {ELEMENT_TYPE_R8, BasicTypes::TypeDouble},
            {ELEMENT_TYPE_R4, BasicTypes::TypeSingle},
            {ELEMENT_TYPE_I4, BasicTypes::TypeInt32},
            {ELEMENT_TYPE_U4, BasicTypes::TypeUInt32},
            {ELEMENT_TYPE_I8, BasicTypes::TypeInt64},
            {ELEMENT_TYPE_U8, BasicTypes::TypeUInt64},
            {ELEMENT_TYPE_I2, BasicTypes::TypeInt16},
            {ELEMENT_TYPE_U2, BasicTypes::TypeUInt16}
        };

        auto findType = basicTypesMap.find(elemType);
        if (findType == basicTypesMap.end())
            return E_FAIL;
        resultType = (int32_t)findType->second;

        ToRelease<ICorDebugGenericValue> iCorGenValue;
        IfFailRet(pValue->QueryInterface(IID_ICorDebugGenericValue, (LPVOID *) &iCorGenValue));
        return iCorGenValue->GetValue(resultData);
    }

    HRESULT GetValueByOperandDataType(PVOID valueData, BasicTypes valueType, ICorDebugValue **ppValue, EvalData &ed)
    {
        if (valueType == BasicTypes::TypeString)
        {
            std::string String = to_utf8((WCHAR*)valueData);
            return ed.pEvalHelpers->CreateString(ed.pThread, String, ppValue);
        }

        static std::unordered_map<BasicTypes, CorElementType> basicTypesMap
        {
            {BasicTypes::TypeBoolean, ELEMENT_TYPE_BOOLEAN},
            {BasicTypes::TypeByte, ELEMENT_TYPE_U1},
            {BasicTypes::TypeSByte, ELEMENT_TYPE_I1},
            {BasicTypes::TypeChar, ELEMENT_TYPE_CHAR},
            {BasicTypes::TypeDouble, ELEMENT_TYPE_R8},
            {BasicTypes::TypeSingle, ELEMENT_TYPE_R4},
            {BasicTypes::TypeInt32, ELEMENT_TYPE_I4},
            {BasicTypes::TypeUInt32, ELEMENT_TYPE_U4},
            {BasicTypes::TypeInt64, ELEMENT_TYPE_I8},
            {BasicTypes::TypeUInt64, ELEMENT_TYPE_U8},
            {BasicTypes::TypeInt16, ELEMENT_TYPE_I2},
            {BasicTypes::TypeUInt16, ELEMENT_TYPE_U2}
        };

        auto findType = basicTypesMap.find(valueType);
        if (findType == basicTypesMap.end())
            return E_FAIL;

        return CreatePrimitiveValue(ed.pThread, ppValue, findType->second, valueData);
    }

    HRESULT CallBinaryOperator(const std::string &opName, ICorDebugValue *pValue, ICorDebugValue *pType1Value, ICorDebugValue *pType2Value,
                               ICorDebugValue **pResultValue, EvalData &ed)
    {
        HRESULT Status;
        std::string typeName1;
        CorElementType elemType1;
        IfFailRet(GetArgData(pType1Value, typeName1, elemType1));
        std::string typeName2;
        CorElementType elemType2;
        IfFailRet(GetArgData(pType2Value, typeName2, elemType2));
        // https://docs.microsoft.com/en-us/dotnet/csharp/language-reference/operators/operator-overloading
        // A unary operator has one input parameter. A binary operator has two input parameters. In each case,
        // at least one parameter must have type T or T? where T is the type that contains the operator declaration.
        std::string typeName;
        CorElementType elemType;
        IfFailRet(GetArgData(pValue, typeName, elemType));
        if ((elemType != elemType1 || typeName != typeName1) && (elemType != elemType2 || typeName != typeName2))
            return E_INVALIDARG;

        ToRelease<ICorDebugValue> iCorTypeValue;
        auto CallOperator = [&](std::function<HRESULT(std::vector<Evaluator::ArgElementType>&)> cb)
        {
            ToRelease<ICorDebugFunction> iCorFunc;
            ed.pEvaluator->WalkMethods(pValue, [&](
                bool is_static,
                const std::string &methodName,
                Evaluator::ReturnElementType&,
                std::vector<Evaluator::ArgElementType> &methodArgs,
                Evaluator::GetFunctionCallback getFunction)
            {
                if (!is_static || methodArgs.size() != 2 || opName != methodName ||
                    FAILED(cb(methodArgs)))
                    return S_OK; // Return with success to continue walk.

                IfFailRet(getFunction(&iCorFunc));

                return E_ABORT; // Fast exit from cycle, since we already found iCorFunc.
            });
            if (!iCorFunc)
                return E_INVALIDARG;

            ICorDebugValue *ppArgsValue[] = {pType1Value, pType2Value};
            return ed.pEvalHelpers->EvalFunction(ed.pThread, iCorFunc, nullptr, 0, ppArgsValue, 2, pResultValue, ed.evalFlags);
        };

        // Try execute operator for exact same type as provided values.
        if (SUCCEEDED(CallOperator([&](std::vector<Evaluator::ArgElementType> &methodArgs)
            {
                return elemType1 != methodArgs[0].corType || typeName1 != methodArgs[0].typeName ||
                       elemType2 != methodArgs[1].corType || typeName2 != methodArgs[1].typeName
                       ? E_FAIL : S_OK;
            })))
            return S_OK;

        // Try execute operator with implicit cast for second value.
        // Make sure we don't cast "base" struct/class value for this case, since "... at least one parameter must have type T...".
        if (elemType == elemType1 && typeName == typeName1 &&
            SUCCEEDED(CallOperator([&](std::vector<Evaluator::ArgElementType> &methodArgs)
            {
                if (elemType1 != methodArgs[0].corType || typeName1 != methodArgs[0].typeName)
                    return E_FAIL;

                ToRelease<ICorDebugValue> iCorResultValue;
                if (FAILED(CallCastOperator("op_Implicit", pType1Value, methodArgs[1].corType, methodArgs[1].typeName, pType2Value, &iCorResultValue, ed)) &&
                    FAILED(CallCastOperator("op_Implicit", pType2Value, methodArgs[1].corType, methodArgs[1].typeName, pType2Value, &iCorResultValue, ed)))
                    return E_FAIL;

                IfFailRet(GetRealValueWithType(iCorResultValue, &iCorTypeValue));
                pType2Value = iCorTypeValue.GetPtr();

                return S_OK;
            })))
            return S_OK;

        // Try execute operator with implicit cast for first value.
        return CallOperator([&](std::vector<Evaluator::ArgElementType> &methodArgs)
            {
                if (elemType2 != methodArgs[1].corType || typeName2 != methodArgs[1].typeName)
                    return E_FAIL;

                ToRelease<ICorDebugValue> iCorResultValue;
                if (FAILED(CallCastOperator("op_Implicit", pType1Value, methodArgs[0].corType, methodArgs[0].typeName, pType1Value, &iCorResultValue, ed)) &&
                    FAILED(CallCastOperator("op_Implicit", pType2Value, methodArgs[0].corType, methodArgs[0].typeName, pType1Value, &iCorResultValue, ed)))
                    return E_FAIL;

                iCorTypeValue.Free();
                IfFailRet(GetRealValueWithType(iCorResultValue, &iCorTypeValue));
                pType1Value = iCorTypeValue.GetPtr();

                return S_OK;
            });
    }

    bool SupportedByCalculationDelegateType(CorElementType elemType)
    {
        static std::unordered_set<CorElementType> supportedElementTypes{
            ELEMENT_TYPE_BOOLEAN,
            ELEMENT_TYPE_U1,
            ELEMENT_TYPE_I1,
            ELEMENT_TYPE_CHAR,
            ELEMENT_TYPE_R8,
            ELEMENT_TYPE_R4,
            ELEMENT_TYPE_I4,
            ELEMENT_TYPE_U4,
            ELEMENT_TYPE_I8,
            ELEMENT_TYPE_U8,
            ELEMENT_TYPE_I2,
            ELEMENT_TYPE_U2,
            ELEMENT_TYPE_STRING
        };

        return supportedElementTypes.find(elemType) != supportedElementTypes.end();
    }

    HRESULT CalculateTwoOparands(OperationType opType, std::list<EvalStackEntry> &evalStack, std::string &output, EvalData &ed)
    {
        HRESULT Status;
        ToRelease<ICorDebugValue> iCorValue2;
        IfFailRet(GetFrontStackEntryValue(&iCorValue2, nullptr, evalStack, ed, output));
        evalStack.pop_front();
        ToRelease<ICorDebugValue> iCorRealValue2;
        CorElementType elemType2;
        IfFailRet(GetRealValueWithType(iCorValue2, &iCorRealValue2, &elemType2));

        ToRelease<ICorDebugValue> iCorValue1;
        IfFailRet(GetFrontStackEntryValue(&iCorValue1, nullptr, evalStack, ed, output));
        evalStack.front().ResetEntry();
        ToRelease<ICorDebugValue> iCorRealValue1;
        CorElementType elemType1;
        IfFailRet(GetRealValueWithType(iCorValue1, &iCorRealValue1, &elemType1));

        if (elemType1 == ELEMENT_TYPE_VALUETYPE || elemType2 == ELEMENT_TYPE_VALUETYPE ||
            elemType1 == ELEMENT_TYPE_CLASS || elemType2 == ELEMENT_TYPE_CLASS)
        {
            static std::unordered_map<OperationType, std::pair<std::string,std::string>> opMap{
                {OperationType::AddExpression, {"op_Addition", "+"}},
                {OperationType::SubtractExpression, {"op_Subtraction", "-"}},
                {OperationType::MultiplyExpression, {"op_Multiply", "*"}},
                {OperationType::DivideExpression, {"op_Division", "/"}},
                {OperationType::ModuloExpression, {"op_Modulus", "%"}},
                {OperationType::RightShiftExpression, {"op_RightShift", ">>"}},
                {OperationType::LeftShiftExpression, {"op_LeftShift", "<<"}},
                {OperationType::LogicalAndExpression, {"op_LogicalAnd", "&&"}},
                {OperationType::LogicalOrExpression, {"op_LogicalOr", "||"}},
                {OperationType::ExclusiveOrExpression, {"op_ExclusiveOr", "^"}},
                {OperationType::BitwiseAndExpression, {"op_BitwiseAnd", "&"}},
                {OperationType::BitwiseOrExpression, {"op_BitwiseOr", "|"}},
                {OperationType::EqualsExpression, {"op_Equality", "=="}},
                {OperationType::NotEqualsExpression, {"op_Inequality", "!="}},
                {OperationType::LessThanExpression, {"op_LessThan", "<"}},
                {OperationType::GreaterThanExpression, {"op_GreaterThan", ">"}},
                {OperationType::LessThanOrEqualExpression, {"op_LessThanOrEqual", "<="}},
                {OperationType::GreaterThanOrEqualExpression, {"op_GreaterThanOrEqual", ">="}}
            };

            auto findOpName = opMap.find(opType);
            if (findOpName == opMap.end())
                return E_FAIL;

            if (((elemType1 == ELEMENT_TYPE_VALUETYPE || elemType1 == ELEMENT_TYPE_CLASS) &&
                    SUCCEEDED(CallBinaryOperator(findOpName->second.first, iCorRealValue1, iCorRealValue1, iCorRealValue2, &evalStack.front().iCorValue, ed))) ||
                ((elemType2 == ELEMENT_TYPE_VALUETYPE || elemType2 == ELEMENT_TYPE_CLASS) &&
                    SUCCEEDED(CallBinaryOperator(findOpName->second.first, iCorRealValue2, iCorRealValue1, iCorRealValue2, &evalStack.front().iCorValue, ed))))
                return S_OK;

            std::string typeRetName;
            CorElementType elemRetType;
            ToRelease<ICorDebugValue> iCorResultValue;
            // Try to implicitly cast struct/class object into build-in type supported by CalculationDelegate().
            if (SupportedByCalculationDelegateType(elemType2) && // First is ELEMENT_TYPE_VALUETYPE or ELEMENT_TYPE_CLASS
                SUCCEEDED(GetArgData(iCorRealValue2, typeRetName, elemRetType)) &&
                SUCCEEDED(CallCastOperator("op_Implicit", iCorRealValue1, elemRetType, typeRetName, iCorRealValue1, &iCorResultValue, ed)))
            {
                iCorRealValue1.Free();
                IfFailRet(GetRealValueWithType(iCorResultValue, &iCorRealValue1, &elemType1));
                // goto CalculationDelegate() related routine (see code below this 'if' statement scope)
            }
            else if (SupportedByCalculationDelegateType(elemType1) && // Second is ELEMENT_TYPE_VALUETYPE or ELEMENT_TYPE_CLASS
                     SUCCEEDED(GetArgData(iCorRealValue1, typeRetName, elemRetType)) &&
                     SUCCEEDED(CallCastOperator("op_Implicit", iCorRealValue2, elemRetType, typeRetName, iCorRealValue2, &iCorResultValue, ed)))
            {
                iCorRealValue2.Free();
                IfFailRet(GetRealValueWithType(iCorResultValue, &iCorRealValue2, &elemType2));
                // goto CalculationDelegate() related routine (see code below this 'if' statement scope)
            }
            else
            {
                std::string typeName1;
                IfFailRet(TypePrinter::GetTypeOfValue(iCorRealValue1, typeName1));
                std::string typeName2;
                IfFailRet(TypePrinter::GetTypeOfValue(iCorRealValue2, typeName2));
                output = "error CS0019: Operator '" + findOpName->second.second + "' cannot be applied to operands of type '" + typeName1 + "' and '" + typeName2 + "'";
                return E_INVALIDARG;
            }
        }
        else if (!SupportedByCalculationDelegateType(elemType1) || !SupportedByCalculationDelegateType(elemType2))
            return E_INVALIDARG;

        int64_t valueDataHolder1 = 0;
        PVOID valueData1 = &valueDataHolder1;
        int32_t valueType1 = 0;
        int64_t valueDataHolder2 = 0;
        PVOID valueData2 = &valueDataHolder2;
        int32_t valueType2 = 0;
        PVOID resultData = NULL;
        int32_t resultType = 0;
        if (SUCCEEDED(Status = GetOperandDataTypeByValue(iCorRealValue1, elemType1, valueData1, valueType1)) &&
            SUCCEEDED(Status = GetOperandDataTypeByValue(iCorRealValue2, elemType2, valueData2, valueType2)) &&
            SUCCEEDED(Status = Interop::CalculationDelegate(valueData1, valueType1, valueData2, valueType2, (int32_t)opType, resultType, &resultData, output)))
        {
            Status = GetValueByOperandDataType(resultData, (BasicTypes)resultType, &evalStack.front().iCorValue, ed);
            if (resultType == (int32_t)BasicTypes::TypeString)
                Interop::SysFreeString((BSTR)resultData);
            else
                Interop::CoTaskMemFree(resultData);
        }

        if (valueType1 == (int32_t)BasicTypes::TypeString && valueData1)
            Interop::SysFreeString((BSTR)valueData1);

        if (valueType2 == (int32_t)BasicTypes::TypeString && valueData2)
            Interop::SysFreeString((BSTR)valueData2);

        return Status;
    }

    HRESULT CalculateOneOparand(OperationType opType, std::list<EvalStackEntry> &evalStack, std::string &output, EvalData &ed)
    {
        HRESULT Status;
        ToRelease<ICorDebugValue> iCorValue;
        IfFailRet(GetFrontStackEntryValue(&iCorValue, nullptr, evalStack, ed, output));
        evalStack.front().ResetEntry(EvalStackEntry::ResetLiteralStatus::No);
        ToRelease<ICorDebugValue> iCorRealValue;
        CorElementType elemType;
        IfFailRet(GetRealValueWithType(iCorValue, &iCorRealValue, &elemType));

        if (elemType == ELEMENT_TYPE_VALUETYPE || elemType == ELEMENT_TYPE_CLASS)
        {
            static std::unordered_map<OperationType, std::pair<std::string,std::string>> opMap{
                {OperationType::LogicalNotExpression, {"op_LogicalNot", "!"}},
                {OperationType::BitwiseNotExpression, {"op_OnesComplement", "~"}},
                {OperationType::UnaryPlusExpression, {"op_UnaryPlus", "+"}},
                {OperationType::UnaryMinusExpression, {"op_UnaryNegation", "-"}}
            };

            auto findOpName = opMap.find(opType);
            if (findOpName == opMap.end())
                return E_FAIL;

            if (SUCCEEDED(CallUnaryOperator(findOpName->second.first, iCorRealValue, &evalStack.front().iCorValue, ed)))
                return S_OK;
            else
            {
                std::string typeName;
                IfFailRet(TypePrinter::GetTypeOfValue(iCorRealValue, typeName));
                output = "error CS0023: Operator '" + findOpName->second.second + "' cannot be applied to operand of type '" + typeName + "'";
                return E_INVALIDARG;
            }
        }
        else if (!SupportedByCalculationDelegateType(elemType))
            return E_INVALIDARG;

        int64_t valueDataHolder1 = 0;
        PVOID valueData1 = &valueDataHolder1;
        int32_t valueType1 = 0;
        // Note, we need fake second operand for delegate.
        int64_t fakeValueData2 = 0;
        PVOID resultData = NULL;
        int32_t resultType = 0;
        if (SUCCEEDED(Status = GetOperandDataTypeByValue(iCorRealValue, elemType, valueData1, valueType1)) &&
            SUCCEEDED(Status = Interop::CalculationDelegate(valueData1, valueType1, &fakeValueData2, (int32_t)BasicTypes::TypeInt64, (int32_t)opType, resultType, &resultData, output)))
        {
            Status = GetValueByOperandDataType(resultData, (BasicTypes)resultType, &evalStack.front().iCorValue, ed);
            if (resultType == (int32_t)BasicTypes::TypeString)
                Interop::SysFreeString((BSTR)resultData);
            else
                Interop::CoTaskMemFree(resultData);
        }

        if (valueType1 == (int32_t)BasicTypes::TypeString && valueData1)
            Interop::SysFreeString((BSTR)valueData1);

        return Status;
    }


    HRESULT IdentifierName(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        std::string String = to_utf8(((FormatFS*)pArguments)->wString);
        ReplaceInternalNames(String, true);

        evalStack.emplace_front();
        evalStack.front().identifiers.emplace_back(std::move(String));
        evalStack.front().editable = true;
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
        int32_t Int = ((FormatFI*)pArguments)->Int;
        if (Int < 0)
            return E_INVALIDARG;

        HRESULT Status;
        bool idsEmpty = false;
        bool isInstance = true;
        std::vector<ToRelease<ICorDebugValue>> iCorArgs(Int);
        for (int32_t i = Int - 1; i >= 0; i--)
        {
            IfFailRet(GetFrontStackEntryValue(&iCorArgs[i], nullptr, evalStack, ed, output));
            evalStack.pop_front();
        }

        assert(evalStack.front().identifiers.size() > 0); // We must have at least method name (identifier).

        // TODO local defined function (compiler will create such function with name like `<Calc1>g__Calc2|0_0`)
        std::string funcName = evalStack.front().identifiers.back();
        evalStack.front().identifiers.pop_back();

        if (!evalStack.front().iCorValue && evalStack.front().identifiers.empty())
        {
            std::string methodClass;
            idsEmpty = true;
            IfFailRet(ed.pEvaluator->GetMethodClass(ed.pThread, ed.frameLevel, methodClass, isInstance));
            if (isInstance)
            {
                evalStack.front().identifiers.emplace_back("this");
            }
            else
            {
                // here we add a full qualified "path" separated with dots (aka Class.Subclass.Subclass ..etc) 
                // although <identifiers> usually contains a vector of components of the full name qualification
                // Anyway, our added component will be correctly processed by Evaluator::ResolveIdentifiers() for
                // that case as it seals all the qualification components into one with dots before using them.
                evalStack.front().identifiers.emplace_back(methodClass);
            }
        }

        ToRelease<ICorDebugValue> iCorValue;
        ToRelease<ICorDebugType> iCorType;
        IfFailRet(ed.pEvaluator->ResolveIdentifiers(ed.pThread, ed.frameLevel, evalStack.front().iCorValue, nullptr, evalStack.front().identifiers, &iCorValue, nullptr, &iCorType, ed.evalFlags));

        bool searchStatic = false;
        if (iCorType)
        {
            searchStatic = true;
        }
        else
        {
            CorElementType elemType;
            IfFailRet(iCorValue->GetType(&elemType));

            // Boxing built-in element type into value type in order to call methods.
            auto entry = ed.corElementToValueClassMap.find(elemType);
            if (entry != ed.corElementToValueClassMap.end())
            {
                ULONG32 cbSize;
                IfFailRet(iCorValue->GetSize(&cbSize));
                ArrayHolder<BYTE> elemValue = new (std::nothrow) BYTE[cbSize];
                if (elemValue == nullptr)
                    return E_OUTOFMEMORY;

                memset(elemValue.GetPtr(), 0, cbSize * sizeof(BYTE));

                ToRelease<ICorDebugGenericValue> pGenericValue;
                IfFailRet(iCorValue->QueryInterface(IID_ICorDebugGenericValue, (LPVOID*) &pGenericValue));
                IfFailRet(pGenericValue->GetValue((LPVOID) &(elemValue[0])));

                iCorValue.Free();
                CreateValueType(ed.pEvalWaiter, ed.pThread, entry->second, &iCorValue, elemValue.GetPtr());
            }

            ToRelease<ICorDebugValue2> iCorValue2;
            IfFailRet(iCorValue->QueryInterface(IID_ICorDebugValue2, (LPVOID *) &iCorValue2));
            IfFailRet(iCorValue2->GetExactType(&iCorType));
        }

        std::vector<Evaluator::ArgElementType> funcArgs(Int);
        for (int32_t i = 0; i < Int; ++i)
        {
            ToRelease<ICorDebugValue> iCorValueArg;
            IfFailRet(DereferenceAndUnboxValue(iCorArgs[i].GetPtr(), &iCorValueArg, nullptr));
            IfFailRet(iCorValueArg->GetType(&funcArgs[i].corType));

            if (funcArgs[i].corType == ELEMENT_TYPE_VALUETYPE || funcArgs[i].corType == ELEMENT_TYPE_CLASS)
                IfFailRet(TypePrinter::NameForTypeByValue(iCorValueArg, funcArgs[i].typeName));
        }

        ToRelease<ICorDebugFunction> iCorFunc;
        ed.pEvaluator->WalkMethods(iCorType, [&](
            bool is_static,
            const std::string &methodName,
            Evaluator::ReturnElementType&,
            std::vector<Evaluator::ArgElementType> &methodArgs,
            Evaluator::GetFunctionCallback getFunction)
        {
            if ( (searchStatic && !is_static) || (!searchStatic && is_static && !idsEmpty) ||
                funcArgs.size() != methodArgs.size() || funcName != methodName)
                return S_OK;

            for (size_t i = 0; i < funcArgs.size(); ++i)
            {
                if (funcArgs[i].corType != methodArgs[i].corType ||
                    funcArgs[i].typeName != methodArgs[i].typeName)
                    return S_OK;
            }

            IfFailRet(getFunction(&iCorFunc));
            isInstance = !is_static;

            return E_ABORT; // Fast exit from cycle.
        });
        if (!iCorFunc)
            return E_FAIL;

        evalStack.front().ResetEntry();

        ULONG32 realArgsCount = Int + (isInstance ? 1 : 0);
        std::vector<ICorDebugValue*> iCorValueArgs;
        iCorValueArgs.reserve(realArgsCount);
        if (isInstance)
        {
            iCorValueArgs.emplace_back(iCorValue.GetPtr());
        }
        for (int32_t i = 0; i < Int; i++)
        {
            iCorValueArgs.emplace_back(iCorArgs[i].GetPtr());
        }

        Status = ed.pEvalHelpers->EvalFunction(ed.pThread, iCorFunc, nullptr, 0, iCorValueArgs.data(), realArgsCount, &evalStack.front().iCorValue, ed.evalFlags);

        // CORDBG_S_FUNC_EVAL_HAS_NO_RESULT: Some Func evals will lack a return value, such as those whose return type is void.
        if (Status == CORDBG_S_FUNC_EVAL_HAS_NO_RESULT)
            // We can't create ELEMENT_TYPE_VOID, so, we are forced to use System.Void instead.
            IfFailRet(CreateValueType(ed.pEvalWaiter, ed.pThread, ed.iCorVoidClass, &evalStack.front().iCorValue, nullptr));

        return Status;
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
        IfFailRet(GetIndexesFromStack(indexes, Int, evalStack, ed, output));

        if (evalStack.front().preventBinding)
            return S_OK;

        ToRelease<ICorDebugValue> iCorArrayValue;
        std::unique_ptr<Evaluator::SetterData> setterData;
        IfFailRet(GetFrontStackEntryValue(&iCorArrayValue, &setterData, evalStack, ed, output));

        evalStack.front().iCorValue.Free();
        evalStack.front().identifiers.clear();
        evalStack.front().setterData = std::move(setterData);
        return ed.pEvaluator->GetElement(iCorArrayValue, indexes, &evalStack.front().iCorValue);
    }

    HRESULT ElementBindingExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        int32_t Int = ((FormatFI*)pArguments)->Int;

        HRESULT Status;
        std::vector<ULONG32> indexes;
        IfFailRet(GetIndexesFromStack(indexes, Int, evalStack, ed, output));

        if (evalStack.front().preventBinding)
            return S_OK;

        ToRelease<ICorDebugValue> iCorArrayValue;
        std::unique_ptr<Evaluator::SetterData> setterData;
        IfFailRet(GetFrontStackEntryValue(&iCorArrayValue, &setterData, evalStack, ed, output));

        ToRelease<ICorDebugReferenceValue> pReferenceValue;
        IfFailRet(iCorArrayValue->QueryInterface(IID_ICorDebugReferenceValue, (LPVOID*) &pReferenceValue));
        BOOL isNull = FALSE;
        IfFailRet(pReferenceValue->IsNull(&isNull));

        if (isNull == TRUE)
        {
            evalStack.front().preventBinding = true;
            return S_OK;
        }

        evalStack.front().iCorValue.Free();
        evalStack.front().identifiers.clear();
        evalStack.front().setterData = std::move(setterData);
        return ed.pEvaluator->GetElement(iCorArrayValue, indexes, &evalStack.front().iCorValue);
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
        evalStack.front().literal = true;
        if (BasicTypesAlias[Int] == ELEMENT_TYPE_VALUETYPE)
            return CreateValueType(ed.pEvalWaiter, ed.pThread, ed.iCorDecimalClass, &evalStack.front().iCorValue, Ptr);
        else
            return CreatePrimitiveValue(ed.pThread, &evalStack.front().iCorValue, BasicTypesAlias[Int], Ptr);
    }

    HRESULT StringLiteralExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        std::string String = to_utf8(((FormatFS*)pArguments)->wString);
        ReplaceInternalNames(String, true);
        evalStack.emplace_front();
        evalStack.front().literal = true;
        return ed.pEvalHelpers->CreateString(ed.pThread, String, &evalStack.front().iCorValue);
    }

    HRESULT CharacterLiteralExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        PVOID Ptr = ((FormatFIP*)pArguments)->Ptr;
        evalStack.emplace_front();
        evalStack.front().literal = true;
        return CreatePrimitiveValue(ed.pThread, &evalStack.front().iCorValue, ELEMENT_TYPE_CHAR, Ptr);
    }

    HRESULT PredefinedType(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        static const CorElementType BasicTypesAlias[] {
            ELEMENT_TYPE_BOOLEAN,   // Boolean
            ELEMENT_TYPE_U1,        // Byte
            ELEMENT_TYPE_CHAR,      // Char
            ELEMENT_TYPE_VALUETYPE, // Decimal
            ELEMENT_TYPE_R8,        // Double
            ELEMENT_TYPE_R4,        // Float
            ELEMENT_TYPE_I4,        // Int
            ELEMENT_TYPE_I8,        // Long
            ELEMENT_TYPE_MAX,       // Object
            ELEMENT_TYPE_I1,        // SByte
            ELEMENT_TYPE_I2,        // Short
            ELEMENT_TYPE_MAX,       // String
            ELEMENT_TYPE_U2,        // UShort
            ELEMENT_TYPE_U4,        // UInt
            ELEMENT_TYPE_U8         // ULong
        };

        // TODO uint32_t Flags = ((FormatFI*)pArguments)->Flags;
        int32_t Int = ((FormatFI*)pArguments)->Int;

        evalStack.emplace_front();

        if (BasicTypesAlias[Int] == ELEMENT_TYPE_VALUETYPE)
            return CreateValueType(ed.pEvalWaiter, ed.pThread, ed.iCorDecimalClass, &evalStack.front().iCorValuePredefined, nullptr);
        else
            return CreatePrimitiveValue(ed.pThread, &evalStack.front().iCorValuePredefined, BasicTypesAlias[Int], nullptr);
    }

    HRESULT AliasQualifiedName(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT MemberBindingExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        assert(evalStack.size() > 1);
        assert(evalStack.front().identifiers.size() == 1); // Only one unresolved identifier must be here.
        assert(!evalStack.front().iCorValue); // Should be unresolved identifier only front element.

        std::string identifier = std::move(evalStack.front().identifiers[0]);
        evalStack.pop_front();

        if (evalStack.front().preventBinding)
            return S_OK;

        HRESULT Status;
        ToRelease<ICorDebugValue> iCorValue;
        std::unique_ptr<Evaluator::SetterData> setterData;
        IfFailRet(GetFrontStackEntryValue(&iCorValue, &setterData, evalStack, ed, output));
        evalStack.front().iCorValue = iCorValue.Detach();
        evalStack.front().identifiers.clear();
        evalStack.front().setterData = std::move(setterData);

        ToRelease<ICorDebugReferenceValue> pReferenceValue;
        IfFailRet(evalStack.front().iCorValue->QueryInterface(IID_ICorDebugReferenceValue, (LPVOID*) &pReferenceValue));
        BOOL isNull = FALSE;
        IfFailRet(pReferenceValue->IsNull(&isNull));

        if (isNull == TRUE)
            evalStack.front().preventBinding = true;
        else
            evalStack.front().identifiers.emplace_back(std::move(identifier));

        return S_OK;
    }

    HRESULT ConditionalExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT SimpleMemberAccessExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        assert(evalStack.size() > 1);
        assert(!evalStack.front().iCorValue); // Should be unresolved identifier only front element.
        assert(evalStack.front().identifiers.size() == 1); // Only one unresolved identifier must be here.

        std::string identifier = std::move(evalStack.front().identifiers[0]);
        evalStack.pop_front();

        if (!evalStack.front().preventBinding)
            evalStack.front().identifiers.emplace_back(std::move(identifier));

        return S_OK;
    }

    HRESULT QualifiedName(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        return SimpleMemberAccessExpression(evalStack, pArguments, output, ed);
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
        return CalculateTwoOparands(OperationType::AddExpression, evalStack, output, ed);
    }

    HRESULT MultiplyExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        return CalculateTwoOparands(OperationType::MultiplyExpression, evalStack, output, ed);
    }

    HRESULT SubtractExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        return CalculateTwoOparands(OperationType::SubtractExpression, evalStack, output, ed);
    }

    HRESULT DivideExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        return CalculateTwoOparands(OperationType::DivideExpression, evalStack, output, ed);
    }

    HRESULT ModuloExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        return CalculateTwoOparands(OperationType::ModuloExpression, evalStack, output, ed);
    }

    HRESULT LeftShiftExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        return CalculateTwoOparands(OperationType::LeftShiftExpression, evalStack, output, ed);
    }

    HRESULT RightShiftExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        return CalculateTwoOparands(OperationType::RightShiftExpression, evalStack, output, ed);
    }

    HRESULT BitwiseAndExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        return CalculateTwoOparands(OperationType::BitwiseAndExpression, evalStack, output, ed);
    }

    HRESULT BitwiseOrExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        return CalculateTwoOparands(OperationType::BitwiseOrExpression, evalStack, output, ed);
    }

    HRESULT ExclusiveOrExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        return CalculateTwoOparands(OperationType::ExclusiveOrExpression, evalStack, output, ed);
    }

    HRESULT LogicalAndExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        return CalculateTwoOparands(OperationType::LogicalAndExpression, evalStack, output, ed);
    }

    HRESULT LogicalOrExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        return CalculateTwoOparands(OperationType::LogicalOrExpression, evalStack, output, ed);
    }

    HRESULT EqualsExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        return CalculateTwoOparands(OperationType::EqualsExpression, evalStack, output, ed);
    }

    HRESULT NotEqualsExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        return CalculateTwoOparands(OperationType::NotEqualsExpression, evalStack, output, ed);
    }

    HRESULT GreaterThanExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        return CalculateTwoOparands(OperationType::GreaterThanExpression, evalStack, output, ed);
    }

    HRESULT LessThanExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        return CalculateTwoOparands(OperationType::LessThanExpression, evalStack, output, ed);
    }

    HRESULT GreaterThanOrEqualExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        return CalculateTwoOparands(OperationType::GreaterThanOrEqualExpression, evalStack, output, ed);
    }

    HRESULT LessThanOrEqualExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        return CalculateTwoOparands(OperationType::LessThanOrEqualExpression, evalStack, output, ed);
    }

    HRESULT IsExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT UnaryPlusExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        return CalculateOneOparand(OperationType::UnaryPlusExpression, evalStack, output, ed);
    }

    HRESULT UnaryMinusExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        return CalculateOneOparand(OperationType::UnaryMinusExpression, evalStack, output, ed);
    }

    HRESULT LogicalNotExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        return CalculateOneOparand(OperationType::LogicalNotExpression, evalStack, output, ed);
    }

    HRESULT BitwiseNotExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        return CalculateOneOparand(OperationType::BitwiseNotExpression, evalStack, output, ed);
    }

    HRESULT TrueLiteralExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        evalStack.emplace_front();
        evalStack.front().literal = true;
        return CreateBooleanValue(ed.pThread, &evalStack.front().iCorValue, true);
    }

    HRESULT FalseLiteralExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        evalStack.emplace_front();
        evalStack.front().literal = true;
        return CreateBooleanValue(ed.pThread, &evalStack.front().iCorValue, false);
    }

    HRESULT NullLiteralExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        evalStack.emplace_front();
        evalStack.front().literal = true;
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
        assert(evalStack.size() > 0);
        HRESULT Status;
        uint32_t size = 0;
        PVOID szPtr = &size;

        if (evalStack.front().iCorValuePredefined)
        {
            //  predefined type
            CorElementType elType;
            IfFailRet(evalStack.front().iCorValuePredefined->GetType(&elType));
            if(elType == ELEMENT_TYPE_CLASS)
            {
                ToRelease<ICorDebugValue> iCorValue;
                IfFailRet(DereferenceAndUnboxValue(evalStack.front().iCorValuePredefined, &iCorValue, nullptr));
                IfFailRet(iCorValue->GetSize(&size));
            }
            else
            {
                IfFailRet(evalStack.front().iCorValuePredefined->GetSize(&size));
            }
        }
        else
        {
            ToRelease<ICorDebugType> iCorType;
            ToRelease<ICorDebugValue> iCorValueRef, iCorValue;

            IfFailRet(GetFrontStackEntryType(&iCorType, evalStack, ed, output));
            if(iCorType)
            {
                CorElementType elType;
                IfFailRet(iCorType->GetType(&elType));
                if (elType == ELEMENT_TYPE_VALUETYPE)
                {
                    // user defined type (structure)
                    ToRelease<ICorDebugClass> iCorClass;

                    IfFailRet(iCorType->GetClass(&iCorClass));
                    IfFailRet(CreateValueType(ed.pEvalWaiter, ed.pThread, iCorClass, &iCorValueRef, nullptr));
                    IfFailRet(DereferenceAndUnboxValue(iCorValueRef, &iCorValue, nullptr));
                    IfFailRet(iCorValue->GetSize(&size));
                }
                else
                {
                    return E_INVALIDARG;
                }
            }
            else
            {
                // TODO other cases
                return E_NOTIMPL;
            }
        }
        evalStack.front().ResetEntry();
        return CreatePrimitiveValue(ed.pThread, &evalStack.front().iCorValue, ELEMENT_TYPE_U4, szPtr);
    }


    HRESULT TypeOfExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        // TODO uint32_t Flags = ((FormatF*)pArguments)->Flags;
        return E_NOTIMPL;
    }

    HRESULT CoalesceExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        HRESULT Status;
        ToRelease<ICorDebugValue> iCorRealValueRightOp;
        ToRelease<ICorDebugValue> iCorRightOpValue;
        CorElementType elemTypeRightOp;
        IfFailRet(GetFrontStackEntryValue(&iCorRightOpValue, nullptr, evalStack, ed, output));
        IfFailRet(GetRealValueWithType(iCorRightOpValue, &iCorRealValueRightOp, &elemTypeRightOp));
        auto rightOperand = std::move(evalStack.front());
        evalStack.pop_front();

        ToRelease<ICorDebugValue> iCorRealValueLeftOp;
        ToRelease<ICorDebugValue> iCorLeftOpValue;
        CorElementType elemTypeLeftOp;
        IfFailRet(GetFrontStackEntryValue(&iCorLeftOpValue, nullptr, evalStack, ed, output));
        IfFailRet(GetRealValueWithType(iCorLeftOpValue, &iCorRealValueLeftOp, &elemTypeLeftOp));
        std::string typeNameLeft;
        std::string typeNameRigth;

        //TODO add implementation for object type ?? other
        if((elemTypeRightOp == ELEMENT_TYPE_STRING && elemTypeLeftOp == ELEMENT_TYPE_STRING)
           || ((elemTypeRightOp == ELEMENT_TYPE_CLASS && elemTypeLeftOp == ELEMENT_TYPE_CLASS)
              && SUCCEEDED(TypePrinter::NameForTypeByValue(iCorRealValueLeftOp, typeNameLeft))
              && SUCCEEDED(TypePrinter::NameForTypeByValue(iCorRealValueRightOp, typeNameRigth))
              && typeNameLeft == typeNameRigth))
        {
            ToRelease<ICorDebugReferenceValue> pRefValue;
            IfFailRet(iCorLeftOpValue->QueryInterface(IID_ICorDebugReferenceValue, (LPVOID*) &pRefValue));
            BOOL isNull = FALSE;
            IfFailRet(pRefValue->IsNull(&isNull));

            if (isNull == TRUE)
            {
                evalStack.pop_front();
                evalStack.push_front(std::move(rightOperand));
            }
            return S_OK;
        }
        //TODO add proccesing for parent-child class relationship
        std::string typeName1;
        std::string typeName2;
        IfFailRet(TypePrinter::GetTypeOfValue(iCorRealValueLeftOp, typeName1));
        IfFailRet(TypePrinter::GetTypeOfValue(iCorRealValueRightOp, typeName2));
        output = "error CS0019: Operator ?? cannot be applied to operands of type '" + typeName1 + "' and '" + typeName2 + "'";
        return E_INVALIDARG;
    }

    HRESULT ThisExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        evalStack.emplace_front();
        evalStack.front().identifiers.emplace_back("this");
        evalStack.front().editable = true;
        return S_OK;
    }

} // unnamed namespace

HRESULT EvalStackMachine::Run(ICorDebugThread *pThread, FrameLevel frameLevel, int evalFlags, const std::string &expression,
                              std::list<EvalStackEntry> &evalStack, std::string &output)
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
    IfFailRet(Interop::GenerateStackMachineProgram(fixed_expression, &pStackProgram, output));

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
            FAILED(Status = CommandImplementation[Command](evalStack, pArguments, output, m_evalData)))
            break;
    }
    while (1);

    Interop::ReleaseStackMachineProgram(pStackProgram);
    return Status;
}

HRESULT EvalStackMachine::EvaluateExpression(ICorDebugThread *pThread, FrameLevel frameLevel, int evalFlags, const std::string &expression, ICorDebugValue **ppResultValue,
                                             std::string &output, bool *editable, std::unique_ptr<Evaluator::SetterData> *resultSetterData)
{
    HRESULT Status;
    std::list<EvalStackEntry> evalStack;
    IfFailRet(Run(pThread, frameLevel, evalFlags, expression, evalStack, output));

    assert(evalStack.size() == 1);

    std::unique_ptr<Evaluator::SetterData> setterData;
    IfFailRet(GetFrontStackEntryValue(ppResultValue, &setterData, evalStack, m_evalData, output));

    if (editable)
        *editable = setterData.get() && !setterData.get()->setterFunction ?
                    false /*property don't have setter*/ : evalStack.front().editable;

    if (resultSetterData)
        *resultSetterData = std::move(setterData);

    return S_OK;
}

HRESULT EvalStackMachine::SetValueByExpression(ICorDebugThread *pThread, FrameLevel frameLevel, int evalFlags, ICorDebugValue *pValue,
                                               const std::string &expression, std::string &output)
{
    HRESULT Status;
    std::list<EvalStackEntry> evalStack;
    IfFailRet(Run(pThread, frameLevel, evalFlags, expression, evalStack, output));

    assert(evalStack.size() == 1);

    ToRelease<ICorDebugValue> iCorValue;
    IfFailRet(GetFrontStackEntryValue(&iCorValue, nullptr, evalStack, m_evalData, output));

    return ImplicitCast(iCorValue, pValue, evalStack.front().literal, m_evalData);
}

HRESULT EvalStackMachine::FindPredefinedTypes(ICorDebugModule *pModule)
{
    HRESULT Status;
    ToRelease<IUnknown> pMDUnknown;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    mdTypeDef typeDef = mdTypeDefNil;
    static const WCHAR strTypeDefDecimal[] = W("System.Decimal");
    IfFailRet(pMD->FindTypeDefByName(strTypeDefDecimal, NULL, &typeDef));
    IfFailRet(pModule->GetClassFromToken(typeDef, &m_evalData.iCorDecimalClass));

    typeDef = mdTypeDefNil;
    static const WCHAR strTypeDefVoid[] = W("System.Void");
    IfFailRet(pMD->FindTypeDefByName(strTypeDefVoid, NULL, &typeDef));
    IfFailRet(pModule->GetClassFromToken(typeDef, &m_evalData.iCorVoidClass));

    static const std::vector<std::pair<CorElementType, const WCHAR*>> corElementToValueNameMap{
        {ELEMENT_TYPE_BOOLEAN,  W("System.Boolean")},
        {ELEMENT_TYPE_CHAR,     W("System.Char")},
        {ELEMENT_TYPE_I1,       W("System.SByte")},
        {ELEMENT_TYPE_U1,       W("System.Byte")},
        {ELEMENT_TYPE_I2,       W("System.Int16")},
        {ELEMENT_TYPE_U2,       W("System.UInt16")},
        {ELEMENT_TYPE_I4,       W("System.Int32")},
        {ELEMENT_TYPE_U4,       W("System.UInt32")},
        {ELEMENT_TYPE_I8,       W("System.Int64")},
        {ELEMENT_TYPE_U8,       W("System.UInt64")},
        {ELEMENT_TYPE_R4,       W("System.Single")},
        {ELEMENT_TYPE_R8,       W("System.Double")}
    };

    for (auto &entry : corElementToValueNameMap)
    {
        typeDef = mdTypeDefNil;
        IfFailRet(pMD->FindTypeDefByName(entry.second, NULL, &typeDef));
        IfFailRet(pModule->GetClassFromToken(typeDef, &m_evalData.corElementToValueClassMap[entry.first]));
    }

    return S_OK;
}

} // namespace netcoredbg
