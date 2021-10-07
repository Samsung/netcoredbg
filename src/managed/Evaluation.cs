// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Microsoft.CodeAnalysis.CSharp.Scripting;
using Microsoft.CodeAnalysis.Scripting;
using Microsoft.CodeAnalysis;
using System.Reflection;
using System.Dynamic;
using Microsoft.CodeAnalysis.CSharp;
using System.Text;

namespace NetCoreDbg
{
    public partial class Evaluation
    {
        //BasicTypes enum must be sync with enum from native part
        internal enum BasicTypes
        {
            TypeCorValue = -1,
            TypeObject = 0,
            TypeBoolean,
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
            TypeIntPtr,
            TypeUIntPtr,
            TypeDecimal,
            TypeString,
        };

        //OperationType enum must be sync with enum from native part
        internal enum OperationType
        {
            Addition = 1,
            Subtraction,
            Multiplication,
            Division,
            Remainder,
            BitwiseRightShift,
            BitwiseLeftShift,
            BitwiseComplement,
            LogicalAnd,
            LogicalOR,
            LogicalXOR,
            ConditionalLogicalAnd,
            ConditionalLogicalOR,
            LogicalNegation,
            Equality,
            Inequality,
            LessThan,
            GreaterThan,
            LessThanOrEqual,
            GreaterThanOrEqual
        };

        internal static Dictionary<OperationType, Func<object, object, object>> operationTypesMap = new Dictionary<OperationType, Func<object, object, object>>
        {
            { OperationType.Addition, (object firstOp, object secondOp) => { return Addition(firstOp, secondOp); }},
            { OperationType.Division, (object firstOp, object secondOp) => { return Division(firstOp, secondOp); }},
            { OperationType.Multiplication, (object firstOp, object secondOp) => { return Multiplication(firstOp, secondOp); }},
            { OperationType.Remainder, (object firstOp, object secondOp) => { return Remainder(firstOp, secondOp); }},
            { OperationType.Subtraction, (object firstOp, object secondOp) => { return Subtraction(firstOp, secondOp); }},
            { OperationType.BitwiseRightShift, (object firstOp, object secondOp) => { return BitwiseRightShift(firstOp, secondOp); }},
            { OperationType.BitwiseLeftShift, (object firstOp, object secondOp) => { return BitwiseLeftShift(firstOp, secondOp); }},
            { OperationType.BitwiseComplement, (object firstOp, object secondOp) => { return BitwiseComplement(firstOp); }},
            { OperationType.LogicalAnd, (object firstOp, object secondOp) => { return LogicalAnd(firstOp, secondOp); }},
            { OperationType.LogicalOR, (object firstOp, object secondOp) => { return LogicalOR(firstOp, secondOp); }},
            { OperationType.LogicalXOR, (object firstOp, object secondOp) => { return LogicalXOR(firstOp, secondOp); }},
            { OperationType.ConditionalLogicalAnd, (object firstOp, object secondOp) => { return ConditionalLogicalAnd(firstOp, secondOp); }},
            { OperationType.ConditionalLogicalOR, (object firstOp, object secondOp) => { return ConditionalLogicalOR(firstOp, secondOp); }},
            { OperationType.LogicalNegation, (object firstOp, object secondOp) => { return LogicalNegation(firstOp); }},
            { OperationType.Equality, (object firstOp, object secondOp) => { return Equality(firstOp, secondOp); }},
            { OperationType.Inequality, (object firstOp, object secondOp) => { return Inequality(firstOp, secondOp); }},
            { OperationType.LessThan, (object firstOp, object secondOp) => { return LessThan(firstOp, secondOp); }},
            { OperationType.GreaterThan, (object firstOp, object secondOp) => { return GreaterThan(firstOp, secondOp); }},
            { OperationType.LessThanOrEqual, (object firstOp, object secondOp) => { return LessThanOrEqual(firstOp, secondOp); }},
            { OperationType.GreaterThanOrEqual, (object firstOp, object secondOp) => { return GreaterThanOrEqual(firstOp, secondOp); }}
        };

        internal static Dictionary<BasicTypes, Func<byte[], object>> typesMap = new Dictionary<BasicTypes, Func<byte[], object>>
        {
            { BasicTypes.TypeBoolean, (byte[] values) => {return BitConverter.ToBoolean(values,0);}},
            { BasicTypes.TypeByte, (byte[] values) => {return values[0];}},
            { BasicTypes.TypeChar, (byte[] values) => {return BitConverter.ToChar(values,0);}},
            { BasicTypes.TypeDouble, (byte[] values) => {return BitConverter.ToDouble(values,0);}},
            { BasicTypes.TypeInt16, (byte[] values) => {return BitConverter.ToInt16(values,0);}},
            { BasicTypes.TypeInt32, (byte[] values) => {return BitConverter.ToInt32(values,0);}},
            { BasicTypes.TypeInt64, (byte[] values) => {return BitConverter.ToInt64(values,0);}},
            { BasicTypes.TypeSByte, (byte[] values) => {return (sbyte)values[0];}},
            { BasicTypes.TypeSingle, (byte[] values) => {return BitConverter.ToSingle(values,0);}},
            { BasicTypes.TypeUInt16, (byte[] values) => {return BitConverter.ToUInt16(values,0);}},
            { BasicTypes.TypeUInt32, (byte[] values) => {return BitConverter.ToUInt32(values,0);}},
            { BasicTypes.TypeUInt64, (byte[] values) => {return BitConverter.ToUInt64(values,0);}}
        };

        internal static Dictionary<Type, BasicTypes> basicTypesMap = new Dictionary<Type, BasicTypes>
        {
            { typeof(Boolean), BasicTypes.TypeBoolean },
            { typeof(Byte), BasicTypes.TypeByte },
            { typeof(Char), BasicTypes.TypeChar },
            { typeof(Double), BasicTypes.TypeDouble },
            { typeof(Int16), BasicTypes.TypeInt16 },
            { typeof(Int32), BasicTypes.TypeInt32 },
            { typeof(Int64), BasicTypes.TypeInt64 },
            { typeof(SByte), BasicTypes.TypeSByte },
            { typeof(Single), BasicTypes.TypeSingle },
            { typeof(UInt16), BasicTypes.TypeUInt16 },
            { typeof(UInt32), BasicTypes.TypeUInt32 },
            { typeof(UInt64), BasicTypes.TypeUInt64 }
        };

        /// <summary>
        /// Convert Single(float) type to Int32
        /// </summary>
        /// <param name="value">float value</param>
        /// <returns></returns>
        private static unsafe int floatToInt32Bits(float value)
        {
            return *((int*)&value);
        }

        /// <summary>
        /// Converts value ​​to a IntPtr to IntPtr
        /// </summary>
        /// <param name="value">source value</param>
        /// <returns></returns>
        private static IntPtr valueToPtr(object value) 
        {
            IntPtr result = IntPtr.Zero;
            IntPtr ptr = IntPtr.Zero;
            Int64 newValue = 0;
            if (value.GetType() == typeof(string))
            {
                result = Marshal.AllocHGlobal(Marshal.SizeOf(newValue));
                ptr = Marshal.StringToBSTR(value as string);
            }
            else 
            {
                if (value.GetType() == typeof(float) || value.GetType() == typeof(double))
                    newValue = value.GetType() == typeof(float) ? Convert.ToInt64(floatToInt32Bits(Convert.ToSingle(value))) : BitConverter.DoubleToInt64Bits(Convert.ToDouble(value));
                else
                    newValue = Convert.ToInt64(value);
                var size = Marshal.SizeOf(newValue);
                result = Marshal.AllocHGlobal(Marshal.SizeOf(newValue));
                ptr = Marshal.AllocHGlobal(size);
                Marshal.WriteInt64(ptr, newValue);
            }
            Marshal.WriteIntPtr(result, ptr);
            return result;
        }

        private static object ptrToValue(IntPtr ptr, int type) 
        {
            if ((BasicTypes)type == BasicTypes.TypeString)
                return Marshal.PtrToStringAuto(ptr);

            var intValue = Marshal.ReadInt64(ptr);
            var bytesArray = BitConverter.GetBytes(intValue);
            return typesMap[(BasicTypes)type](bytesArray);
        }

        internal static RetCode CalculationDelegate(IntPtr firstOpPtr, int firstType, IntPtr secondOpPtr, int secondType, int operation, int resultType, out IntPtr result, out IntPtr errorText)
        {
            result = IntPtr.Zero;
            errorText = IntPtr.Zero;

            try
            {
                var firstOp = ptrToValue(firstOpPtr, firstType);
                var secondOp = ptrToValue(secondOpPtr, secondType);
                object operationResult = operationTypesMap[(OperationType)operation](firstOp, secondOp);
                resultType = (int)basicTypesMap[operationResult.GetType()];
                result = valueToPtr(operationResult);
            }
            catch (Microsoft.CSharp.RuntimeBinder.RuntimeBinderException ex)
            {
                errorText = Marshal.StringToBSTR(ex.ToString());
                return RetCode.Exception;
            }
            catch (System.Exception ex)
            {
                errorText = Marshal.StringToBSTR(ex.ToString());
                return RetCode.Exception;
            }

            return RetCode.OK;
        }

        private static object Addition(dynamic first, dynamic second)
        {
            return first + second;
        }

        private static object Subtraction(dynamic first, dynamic second)
        {
            return first - second;
        }

        private static object Multiplication(dynamic first, dynamic second)
        {
            return first * second;
        }

        private static object Division(dynamic first, dynamic second)
        {
            return first / second;
        }

        private static object Remainder(dynamic first, dynamic second)
        {
            return first % second;
        }

        private static object BitwiseRightShift(dynamic first, dynamic second) 
        {
            return first >> second;
        }

        private static object BitwiseLeftShift(dynamic first, dynamic second)
        {
            return first << second;
        }

        private static object BitwiseComplement(dynamic first)
        {
            return ~first;
        }

        private static object LogicalAnd(dynamic first, dynamic second)
        {
            return first & second;
        }

        private static object LogicalOR(dynamic first, dynamic second)
        {
            return first | second;
        }

        private static object LogicalXOR(dynamic first, dynamic second)
        {
            return first ^ second;
        }

        private static bool ConditionalLogicalAnd(dynamic first, dynamic second)
        {
            return first && second;
        }

        private static bool ConditionalLogicalOR(dynamic first, dynamic second)
        {
            return first || second;
        }

        private static object LogicalNegation(dynamic first)
        {
            return !first;
        }

        private static bool Equality(dynamic first, dynamic second)
        {
            return first == second;
        }

        private static bool Inequality(dynamic first, dynamic second) 
        {
            return first != second;
        }

        private static bool LessThan(dynamic first, dynamic second)
        {
            return first < second;
        }

        private static bool GreaterThan(dynamic first, dynamic second)
        {
            return first > second;
        }

        private static bool LessThanOrEqual(dynamic first, dynamic second)
        {
            return first <= second;
        }

        private static bool GreaterThanOrEqual(dynamic first, dynamic second)
        {
            return first >= second;
        }
    }
}
