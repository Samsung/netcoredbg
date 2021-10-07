// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <array>
#include <functional>
#include <sstream>
#include <iterator>
#include <arrayholder.h>
#include "debugger/evalstackmachine.h"
#include "debugger/evaluator.h"
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

    HRESULT GetFrontStackEntryValue(ICorDebugValue **ppResultValue, std::list<EvalStackEntry> &evalStack, EvalData &ed, std::string &output)
    {
        HRESULT Status;
        if (FAILED(Status = ed.pEvaluator->ResolveIdentifiers(ed.pThread, ed.frameLevel, evalStack.front().iCorValue, evalStack.front().identifiers, ppResultValue, nullptr, ed.evalFlags))
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

    HRESULT GetIndexesFromStack(std::vector<ULONG32> &indexes, int dimension, std::list<EvalStackEntry> &evalStack, EvalData &ed, std::string &output)
    {
        HRESULT Status;

        for (int32_t i = 0; i < dimension; i++)
        {
            ToRelease<ICorDebugValue> iCorValue;
            IfFailRet(GetFrontStackEntryValue(&iCorValue, evalStack, ed, output));
            evalStack.pop_front();

            // TODO implicitly convert iCorValue to int, if type not int
            //      at this moment GetElementIndex() work with integer types only

            ULONG32 result_index = 0;
            IfFailRet(GetElementIndex(iCorValue, result_index));
            indexes.insert(indexes.begin(), result_index);
        }

        return S_OK;
    }

    template<typename T1, typename T2>
    HRESULT NumericPromotionWithValue(ICorDebugValue *pInputValue, ICorDebugValue **ppResultValue, EvalData &ed)
    {
        HRESULT Status;
        ToRelease<ICorDebugGenericValue> pGenericValue;
        IfFailRet(pInputValue->QueryInterface(IID_ICorDebugGenericValue, (LPVOID*) &pGenericValue));

        T1 oldTypeValue = 0;
        IfFailRet(pGenericValue->GetValue((LPVOID) &oldTypeValue));
        T2 newTypeValue = oldTypeValue;

        static_assert(std::is_same<T2, int32_t>::value || std::is_same<T2, int64_t>::value, "only int32_t or int64_t allowed");
        CorElementType elemType = std::is_same<T2, int32_t>::value ? ELEMENT_TYPE_I4 : ELEMENT_TYPE_I8;

        return CreatePrimitiveValue(ed.pThread, ppResultValue, elemType, &newTypeValue);
    }

    HRESULT UnaryNumericPromotion(ICorDebugValue *pInputValue, ICorDebugValue **ppResultValue, EvalData &ed)
    {
        HRESULT Status;
        CorElementType elemType;
        IfFailRet(pInputValue->GetType(&elemType));

        // From ECMA-334:
        // Unary numeric promotions
        // Unary numeric promotion occurs for the operands of the predefined +, -, and ~unary operators.
        // Unary numeric promotion simply consists of converting operands of type sbyte, byte, short, ushort, or char to type int.
        // Additionally, for the unary - operator, unary numeric promotion converts operands of type uint to type long.

        switch (elemType)
        {
            case ELEMENT_TYPE_CHAR:
                return NumericPromotionWithValue<uint16_t, int32_t>(pInputValue, ppResultValue, ed);

            case ELEMENT_TYPE_I1:
                return NumericPromotionWithValue<int8_t, int32_t>(pInputValue, ppResultValue, ed);

            case ELEMENT_TYPE_U1:
                return NumericPromotionWithValue<uint8_t, int32_t>(pInputValue, ppResultValue, ed);

            case ELEMENT_TYPE_I2:
                return NumericPromotionWithValue<int16_t, int32_t>(pInputValue, ppResultValue, ed);

            case ELEMENT_TYPE_U2:
                return NumericPromotionWithValue<uint16_t, int32_t>(pInputValue, ppResultValue, ed);

            case ELEMENT_TYPE_U4:
                return NumericPromotionWithValue<uint32_t, int64_t>(pInputValue, ppResultValue, ed);

            default:
                return E_INVALIDARG;
        }
    }

    template<typename T>
    HRESULT InvertNumberValue(ICorDebugValue *pInputValue)
    {
        HRESULT Status;
        ToRelease<ICorDebugGenericValue> pGenericValue;
        IfFailRet(pInputValue->QueryInterface(IID_ICorDebugGenericValue, (LPVOID*) &pGenericValue));

        T value = 0;
        IfFailRet(pGenericValue->GetValue(&value));
        value = -value;
        return pGenericValue->SetValue(&value);
    }

    HRESULT InvertNumber(ICorDebugValue *pValue)
    {
        HRESULT Status;
        CorElementType elemType;
        IfFailRet(pValue->GetType(&elemType));

        switch (elemType)
        {
            case ELEMENT_TYPE_I1:
                return InvertNumberValue<int8_t>(pValue);

            case ELEMENT_TYPE_I2:
                return InvertNumberValue<int16_t>(pValue);

            case ELEMENT_TYPE_I4:
                return InvertNumberValue<int32_t>(pValue);

            case ELEMENT_TYPE_I8:
                return InvertNumberValue<int64_t>(pValue);

            case ELEMENT_TYPE_R4:
                return InvertNumberValue<float>(pValue);

            case ELEMENT_TYPE_R8:
                return InvertNumberValue<double>(pValue);

            default:
                return E_INVALIDARG;
        }
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

    HRESULT CallUnaryOperator(const std::string opName, ICorDebugValue *pValue, ICorDebugValue **pResultValue, EvalData &ed)
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

    HRESULT CallCastOperator(const std::string opName, ICorDebugValue *pValue, ICorDebugValue *pType1Value, ICorDebugValue *pType2Value,
                             ICorDebugValue **pResultValue, EvalData &ed)
    {
        HRESULT Status;
        std::string typeName1;
        CorElementType elemType1;
        IfFailRet(GetArgData(pType1Value, typeName1, elemType1));
        std::string typeName2;
        CorElementType elemType2;
        IfFailRet(GetArgData(pType2Value, typeName2, elemType2));

        ToRelease<ICorDebugFunction> iCorFunc;
        ed.pEvaluator->WalkMethods(pValue, [&](
            bool is_static,
            const std::string &methodName,
            Evaluator::ReturnElementType& methodRet,
            std::vector<Evaluator::ArgElementType> &methodArgs,
            Evaluator::GetFunctionCallback getFunction)
        {
            if (!is_static || methodArgs.size() != 1 || opName != methodName ||
                elemType1 != methodRet.corType || typeName1 != methodRet.typeName ||
                elemType2 != methodArgs[0].corType || typeName2 != methodArgs[0].typeName)
                return S_OK;

            IfFailRet(getFunction(&iCorFunc));

            return E_ABORT; // Fast exit from cycle.
        });
        if (!iCorFunc)
            return E_FAIL;

        return ed.pEvalHelpers->EvalFunction(ed.pThread, iCorFunc, nullptr, 0, &pType2Value, 1, pResultValue, ed.evalFlags);
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

    HRESULT GetRealValueWithType(ICorDebugValue *pValue, ICorDebugValue **ppResultValue, CorElementType &elemType)
    {
        HRESULT Status;
        // Dereference and unbox value, since we need real value.
        ToRelease<ICorDebugValue> iCorRealValue;
        IfFailRet(DereferenceAndUnboxValue(pValue, &iCorRealValue));
        IfFailRet(iCorRealValue->GetType(&elemType));
        // Note, in case of class (string is class), we must use reference instead.
        if (elemType == ELEMENT_TYPE_STRING ||
            elemType == ELEMENT_TYPE_CLASS)
        {
            pValue->AddRef();
            (*ppResultValue) = pValue;
        }
        else
        {
            (*ppResultValue) = iCorRealValue.Detach();
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
        IfFailRet(GetRealValueWithType(pSrcValue, &iCorRealValue1, elemType1));

        ToRelease<ICorDebugValue> iCorRealValue2;
        CorElementType elemType2;
        IfFailRet(GetRealValueWithType(pDstValue, &iCorRealValue2, elemType2));

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
            IfFailRet(GetRealValueWithType(iCorResultValue, &iCorRealValue1, elemType1));

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


    HRESULT IdentifierName(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        std::string String = to_utf8(((FormatFS*)pArguments)->wString);
        ReplaceInternalNames(String, true);

        evalStack.emplace_front();
        evalStack.front().identifiers.emplace_back(std::move(String));
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
        std::vector<ToRelease<ICorDebugValue>> iCorArgs(Int);
        for (int32_t i = Int - 1; i >= 0; i--)
        {
            IfFailRet(GetFrontStackEntryValue(&iCorArgs[i], evalStack, ed, output));
            evalStack.pop_front();
        }

        assert(evalStack.front().identifiers.size() > 0); // We must have at least method name (identifier).

        // TODO local defined function (compiler will create such function with name like `<Calc1>g__Calc2|0_0`)
        std::string funcName = evalStack.front().identifiers.back();
        evalStack.front().identifiers.pop_back();

        if (!evalStack.front().iCorValue && evalStack.front().identifiers.empty())
            evalStack.front().identifiers.emplace_back("this");

        ToRelease<ICorDebugValue> iCorValue;
        ToRelease<ICorDebugType> iCorType;
        IfFailRet(ed.pEvaluator->ResolveIdentifiers(ed.pThread, ed.frameLevel, evalStack.front().iCorValue, evalStack.front().identifiers, &iCorValue, &iCorType, ed.evalFlags));

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
            ToRelease<ICorDebugValue> iCorValue;
            IfFailRet(DereferenceAndUnboxValue(iCorArgs[i].GetPtr(), &iCorValue, nullptr));
            IfFailRet(iCorValue->GetType(&funcArgs[i].corType));

            if (funcArgs[i].corType == ELEMENT_TYPE_VALUETYPE || funcArgs[i].corType == ELEMENT_TYPE_CLASS)
                IfFailRet(TypePrinter::NameForTypeByValue(iCorValue, funcArgs[i].typeName));
        }

        ToRelease<ICorDebugFunction> iCorFunc;
        ed.pEvaluator->WalkMethods(iCorType, [&](
            bool is_static,
            const std::string &methodName,
            Evaluator::ReturnElementType&,
            std::vector<Evaluator::ArgElementType> &methodArgs,
            Evaluator::GetFunctionCallback getFunction)
        {
            if ((searchStatic && !is_static) || (!searchStatic && is_static) ||
                funcArgs.size() != methodArgs.size() || funcName != methodName)
                return S_OK;

            for (size_t i = 0; i < funcArgs.size(); ++i)
            {
                if (funcArgs[i].corType != methodArgs[i].corType ||
                    funcArgs[i].typeName != methodArgs[i].typeName)
                    return S_OK;
            }

            IfFailRet(getFunction(&iCorFunc));

            return E_ABORT; // Fast exit from cycle.
        });
        if (!iCorFunc)
            return E_FAIL;

        evalStack.front().ResetEntry();

        ULONG32 realArgsCount = Int + (searchStatic ? 0 : 1);
        std::vector<ICorDebugValue*> iCorValueArgs;
        iCorValueArgs.reserve(realArgsCount);
        if (!searchStatic)
        {
            iCorValueArgs.emplace_back(iCorValue.GetPtr());
        }
        for (int32_t i = 0; i < Int; i++)
        {
            iCorValueArgs.emplace_back(iCorArgs[i].GetPtr());
        }

        return ed.pEvalHelpers->EvalFunction(ed.pThread, iCorFunc, nullptr, 0, iCorValueArgs.data(), realArgsCount, &evalStack.front().iCorValue, ed.evalFlags);
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
        IfFailRet(GetFrontStackEntryValue(&iCorArrayValue, evalStack, ed, output));

        evalStack.front().ResetEntry();
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
        IfFailRet(GetFrontStackEntryValue(&iCorArrayValue, evalStack, ed, output));

        ToRelease<ICorDebugReferenceValue> pReferenceValue;
        IfFailRet(iCorArrayValue->QueryInterface(IID_ICorDebugReferenceValue, (LPVOID*) &pReferenceValue));
        BOOL isNull = FALSE;
        IfFailRet(pReferenceValue->IsNull(&isNull));

        if (isNull == TRUE)
        {
            evalStack.front().preventBinding = true;
            return S_OK;
        }

        evalStack.front().ResetEntry();
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
        assert(evalStack.size() > 1);
        assert(evalStack.front().identifiers.size() == 1); // Only one unresolved identifier must be here.
        assert(!evalStack.front().iCorValue); // Should be unresolved identifier only front element.

        std::string identifier = std::move(evalStack.front().identifiers[0]);
        evalStack.pop_front();

        if (evalStack.front().preventBinding)
            return S_OK;

        HRESULT Status;
        ToRelease<ICorDebugValue> iCorValue;
        IfFailRet(GetFrontStackEntryValue(&iCorValue, evalStack, ed, output));
        evalStack.front().iCorValue = iCorValue.Detach();
        evalStack.front().identifiers.clear();

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
        HRESULT Status;
        ToRelease<ICorDebugValue> iCorRefValue;
        IfFailRet(GetFrontStackEntryValue(&iCorRefValue, evalStack, ed, output));
        evalStack.front().ResetEntry(true);

        ToRelease<ICorDebugValue> iCorValue;
        IfFailRet(DereferenceAndUnboxValue(iCorRefValue, &iCorValue, nullptr));
        CorElementType elemType;
        IfFailRet(iCorValue->GetType(&elemType));

        switch (elemType)
        {
            case ELEMENT_TYPE_CHAR:
            case ELEMENT_TYPE_I1:
            case ELEMENT_TYPE_U1:
            case ELEMENT_TYPE_I2:
            case ELEMENT_TYPE_U2:
                return UnaryNumericPromotion(iCorValue, &evalStack.front().iCorValue, ed);

            case ELEMENT_TYPE_I4:
            case ELEMENT_TYPE_U4:
            case ELEMENT_TYPE_I8:
            case ELEMENT_TYPE_U8:
            case ELEMENT_TYPE_R4:
            case ELEMENT_TYPE_R8:
                evalStack.front().iCorValue = iCorValue.Detach();
                return S_OK;

            case ELEMENT_TYPE_VALUETYPE:
            case ELEMENT_TYPE_CLASS:
                if (SUCCEEDED(CallUnaryOperator("op_UnaryPlus", iCorValue, &evalStack.front().iCorValue, ed)))
                    return S_OK;
                else
                {
                    std::string typeName;
                    IfFailRet(TypePrinter::NameForTypeByValue(iCorValue, typeName));
                    output = "error CS0023: Operator '+' cannot be applied to operand of type " + typeName;
                    return E_INVALIDARG;
                }

            default:
                return E_INVALIDARG;
        }
    }

    HRESULT UnaryMinusExpression(std::list<EvalStackEntry> &evalStack, PVOID pArguments, std::string &output, EvalData &ed)
    {
        HRESULT Status;
        ToRelease<ICorDebugValue> iCorRefValue;
        IfFailRet(GetFrontStackEntryValue(&iCorRefValue, evalStack, ed, output));
        evalStack.front().ResetEntry(true);

        ToRelease<ICorDebugValue> iCorValue;
        IfFailRet(DereferenceAndUnboxValue(iCorRefValue, &iCorValue, nullptr));
        CorElementType elemType;
        IfFailRet(iCorValue->GetType(&elemType));

        switch (elemType)
        {
            case ELEMENT_TYPE_U8:
                output = "error CS0023: Operator '-' cannot be applied to operand of type 'ulong'";
                return E_INVALIDARG;

            case ELEMENT_TYPE_CHAR:
            case ELEMENT_TYPE_I1:
            case ELEMENT_TYPE_U1:
            case ELEMENT_TYPE_I2:
            case ELEMENT_TYPE_U2:
            case ELEMENT_TYPE_U4:
                IfFailRet(UnaryNumericPromotion(iCorValue, &evalStack.front().iCorValue, ed));
                return InvertNumber(evalStack.front().iCorValue);

            case ELEMENT_TYPE_I4:
            case ELEMENT_TYPE_I8:
            case ELEMENT_TYPE_R4:
            case ELEMENT_TYPE_R8:
                evalStack.front().iCorValue = iCorValue.Detach();
                return InvertNumber(evalStack.front().iCorValue);

            case ELEMENT_TYPE_VALUETYPE:
            case ELEMENT_TYPE_CLASS:
                if (SUCCEEDED(CallUnaryOperator("op_UnaryNegation", iCorValue, &evalStack.front().iCorValue, ed)))
                    return S_OK;
                else
                {
                    std::string typeName;
                    IfFailRet(TypePrinter::NameForTypeByValue(iCorValue, typeName));
                    output = "error CS0023: Operator '-' cannot be applied to operand of type " + typeName;
                    return E_INVALIDARG;
                }

            default:
                return E_INVALIDARG;
        }
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
        evalStack.front().identifiers.emplace_back("this");
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
            FAILED(Status = CommandImplementation[Command](m_evalStack, pArguments, output, m_evalData)))
            break;
    }
    while (1);

    do
    {
        if (FAILED(Status))
            break;

        assert(m_evalStack.size() == 1);

        if (*ppResultValue == nullptr)
        {
            Status = GetFrontStackEntryValue(ppResultValue, m_evalStack, m_evalData, output);
            break;
        }

        ToRelease<ICorDebugValue> iCorValue;
        if (FAILED(Status = GetFrontStackEntryValue(&iCorValue, m_evalStack, m_evalData, output)))
            break;

        Status = ImplicitCast(iCorValue, (*ppResultValue), m_evalStack.front().literal, m_evalData);
    }
    while (0);

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
    IfFailRet(pModule->GetClassFromToken(typeDef, &m_evalData.iCorDecimalClass));

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
