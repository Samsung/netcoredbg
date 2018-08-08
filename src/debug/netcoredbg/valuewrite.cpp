// Copyright (c) 2018 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#ifndef FEATURE_PAL
// Turn off macro definitions named max and min in <windows.h> header file
// to avoid compile error for std::numeric_limits<uint64_t>::max().
#define NOMINMAX
#endif

#include "valuewrite.h"

#include <locale>
#include <codecvt>
#include <cstring>
#include <sstream>
#include <iomanip>

#include <arrayholder.h>

#include "torelease.h"
#include "cputil.h"
#include "typeprinter.h"


PACK_BEGIN struct Decimal {
    unsigned int unused : 16;
    unsigned int exponent : 8;
    unsigned int unused2 : 7;
    unsigned int sign : 1;

    uint32_t hi;
    uint32_t lo;
    uint32_t mid;

    Decimal() : unused(0), exponent(0), unused2(0), sign(0), hi(0), lo(0), mid(0) {}
    Decimal(uint32_t lo, uint32_t mid, uint32_t hi, uint8_t exp, unsigned int sign) : unused(0),
        exponent(exp), unused2(0), sign(sign), hi(hi), lo(lo), mid(mid) {}
    void Mul10();
    void AddInt32(uint32_t val);
    bool Parse(const std::string &value);

private:
    void ShiftLeft();
    void Add(Decimal &d);
    static bool AddCarry(uint32_t &to, uint32_t val);
} PACK_END;

bool Decimal::AddCarry(uint32_t &to, uint32_t val)
{
    uint32_t v = to;
    to += val;
    return (to < v) || (to < val);
}

void Decimal::ShiftLeft()
{
    uint32_t c0 = (lo & 0x80000000) != 0 ? 1u : 0u;
    uint32_t c1 = (mid & 0x80000000) != 0 ? 1u : 0u;

    lo = lo << 1;
    mid = (mid << 1) | c0;
    hi = (hi << 1) | c1;
}

void Decimal::Add(Decimal &d)
{
    if (AddCarry(lo, d.lo))
        if (AddCarry(mid, 1))
            AddCarry(hi, 1);

    if (AddCarry(mid, d.mid))
        AddCarry(hi, 1);

    AddCarry(hi, d.hi);
}

void Decimal::Mul10()
{
    Decimal d(lo, mid, hi, exponent, sign);

    ShiftLeft();
    ShiftLeft();
    Add(d);
    ShiftLeft();
}

void Decimal::AddInt32(uint32_t val)
{
    if (AddCarry(lo, val))
        if (AddCarry(mid, 1))
            AddCarry(hi, 1);
}




const int DECIMAL_PRECISION = 29;

bool Decimal::Parse(const std::string &value)
{
    std::string valueToStore(value);
    bool is_negative = valueToStore.length() > 0 && valueToStore[0] == '-';

    if (is_negative) {
        sign = 1;
        valueToStore.erase(0, 1);
    }
    else
    {
        sign = 0;
    }

    // Cut all meaningless first zeroes
    const auto ix = valueToStore.find_first_not_of('0');
    valueToStore.erase(0, ix);

    const auto dotPos = valueToStore.find(".");
    if (dotPos != std::string::npos)
        valueToStore.erase(dotPos, 1);

    if (dotPos > std::numeric_limits<int>::max())
        return false;

    int scale = static_cast<int>(dotPos);

    const unsigned char* p = reinterpret_cast<const unsigned char*>(valueToStore.c_str());

    if (*p == 0)
    {
        // To avoid risking an app-compat issue with pre 4.5 (where some app was illegally using Reflection to examine the internal scale bits), we'll only force
        // the scale to 0 if the scale was previously positive (previously, such cases were unparsable to a bug.)
        if (scale > 0)
            scale = 0;
    }
    else
    {
        if (scale > DECIMAL_PRECISION)
            return false;

        while (((scale > 0) || ((*p != 0) && (scale > -(DECIMAL_PRECISION - 1)))) &&
               ((hi < 0x19999999) || ((hi == 0x19999999) &&
                                      ((mid < 0x99999999) || ((mid == 0x99999999) &&
                                                              ((lo < 0x99999999) || ((lo == 0x99999999) &&
                                                                                     (*p <= '5'))))))))
        {
            Mul10();

            if (*p != 0)
                AddInt32((uint32_t)(*p++ - '0'));
            scale--;
        }

        if (*p++ >= '5')
        {
            bool round = true;

            if ((*(p - 1) == '5') && ((*(p - 2) % 2) == 0))
            {
                // Check if previous digit is even, only if the when we are unsure whether hows to do
                // Banker's rounding. For digits > 5 we will be rounding up anyway.
                int count = 20; // Look at the next 20 digits to check to round
                while ((*p == '0') && (count != 0))
                {
                    p++;
                    count--;
                }
                if ((*p == '\0') || (count == 0))
                    round = false;// Do nothing
            }

            if (round)
            {
                AddInt32(1);
                if ((hi | mid | lo) == 0)
                {
                    // If we got here, the magnitude portion overflowed and wrapped back to 0 as the magnitude was already at the MaxValue point:
                    //
                    //     79,228,162,514,264,337,593,543,950,335e+X
                    //
                    // Manually force it to the correct result:
                    //
                    //      7,922,816,251,426,433,759,354,395,034e+(X+1)
                    //
                    // This code path can be reached by trying to parse the following as a Decimal:
                    //
                    //      0.792281625142643375935439503355e28
                    //

                    hi = 0x19999999;
                    mid = 0x99999999;
                    lo = 0x9999999A;
                    scale++;
                }
            }
        }
    }

    if (scale > 0)
        return false; // Rounding may have caused its own overflow. For example, parsing "0.792281625142643375935439503355e29" will get here.

    if (scale <= -DECIMAL_PRECISION)
    {
        // Parsing a large scale zero can give you more precision than fits in the decimal.
        // This should only happen for actual zeros or very small numbers that round to zero.
        hi = 0;
        lo = 0;
        mid = 0;
        exponent = DECIMAL_PRECISION - 1;
    }
    else
    {
        exponent = -scale;
    }

    return true;
}

HRESULT WriteValue(ICorDebugValue *pValue, const std::string &value, ICorDebugThread *pThread, Evaluator &evaluator)
{
    HRESULT Status;

    ULONG32 size;
    IfFailRet(pValue->GetSize(&size));

    ArrayHolder<BYTE> valBuf = new (std::nothrow) BYTE[size];
    if (valBuf == nullptr)
        return E_OUTOFMEMORY;

    CorElementType corType;
    IfFailRet(pValue->GetType(&corType));

    if (corType == ELEMENT_TYPE_STRING)
    {
        ToRelease<ICorDebugValue> pNewString;
        IfFailRet(evaluator.CreateString(pThread, value, &pNewString));

        // Switch object addresses
        ToRelease<ICorDebugReferenceValue> pRefNew;
        IfFailRet(pNewString->QueryInterface(IID_ICorDebugReferenceValue, (LPVOID *) &pRefNew));
        ToRelease<ICorDebugReferenceValue> pRefOld;
        IfFailRet(pValue->QueryInterface(IID_ICorDebugReferenceValue, (LPVOID *) &pRefOld));

        CORDB_ADDRESS addr;
        IfFailRet(pRefNew->GetValue(&addr));
        IfFailRet(pRefOld->SetValue(addr));

        return S_OK;
    }

    ToRelease<ICorDebugGenericValue> pGenValue;
    IfFailRet(pValue->QueryInterface(IID_ICorDebugGenericValue, (LPVOID *) &pGenValue));

    std::stringstream ss(value);

    switch (corType)
    {
        case ELEMENT_TYPE_BOOLEAN:
            if (value == "false")
                valBuf[0] = 0;
            else if (value == "true")
                valBuf[0] = 1;
            else
                return E_INVALIDARG;

            break;
        case ELEMENT_TYPE_CHAR:
        {
            auto wval = to_utf16(value);

            if (wval.length() > 1)
                return E_INVALIDARG;

            std::memcpy(&valBuf[0], &wval[0], sizeof(wval[0]));

            break;
        }
        case ELEMENT_TYPE_I1:
        {
            int num;

            ss >> num;
            valBuf[0] = (char)num;
            break;
        }
        case ELEMENT_TYPE_U1:
        {
            int num;

            ss >> num;
            valBuf[0] = (unsigned char)num;
            break;
        }
        case ELEMENT_TYPE_I2:
            ss >> *(short *)&valBuf[0];
            break;
        case ELEMENT_TYPE_U2:
            ss >> *(unsigned short *)&valBuf[0];
            break;
        case ELEMENT_TYPE_I4:
        case ELEMENT_TYPE_I:
            ss >> *(int *)&valBuf[0];
            break;
        case ELEMENT_TYPE_U4:
        case ELEMENT_TYPE_U:
            ss >> *(unsigned int *)&valBuf[0];
            break;
        case ELEMENT_TYPE_I8:
            ss >> *(__int64 *)&valBuf[0];
            break;
        case ELEMENT_TYPE_U8:
            ss >> *(unsigned __int64 *)&valBuf[0];
            break;
        case ELEMENT_TYPE_R4:
            ss >> std::setprecision(8) >> *(float *)&valBuf[0];
            break;
        case ELEMENT_TYPE_R8:
            ss >> std::setprecision(16) >> *(double *)&valBuf[0];
            break;
        case ELEMENT_TYPE_VALUETYPE:
        case ELEMENT_TYPE_CLASS:
        {
            std::string typeName;
            TypePrinter::GetTypeOfValue(pValue, typeName);
            if (typeName == "decimal")
            {
                Decimal d;
                if (!d.Parse(value))
                    return E_INVALIDARG;
                if (sizeof(Decimal) > size)
                    return E_FAIL;
                std::memcpy(&valBuf[0], &d, size);
            }
            break;
        }
        default:
            return S_OK;
    }

    IfFailRet(pGenValue->SetValue((LPVOID) &valBuf[0]));

    return S_OK;
}
