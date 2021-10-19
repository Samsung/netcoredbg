// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace NetCoreDbg
{
    public partial class Evaluation
    {
        //BasicTypes enum must be sync with enum from native part
        internal enum BasicTypes
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
            TypeString,
        };

        //OperationType enum must be sync with enum from native part
        internal enum OperationType
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

        internal static Dictionary<OperationType, Func<object, object, object>> operationTypesMap = new Dictionary<OperationType, Func<object, object, object>>
        {
            { OperationType.AddExpression, (object firstOp, object secondOp) => { return AddExpression(firstOp, secondOp); }},
            { OperationType.DivideExpression, (object firstOp, object secondOp) => { return DivideExpression(firstOp, secondOp); }},
            { OperationType.MultiplyExpression, (object firstOp, object secondOp) => { return MultiplyExpression(firstOp, secondOp); }},
            { OperationType.ModuloExpression, (object firstOp, object secondOp) => { return ModuloExpression(firstOp, secondOp); }},
            { OperationType.SubtractExpression, (object firstOp, object secondOp) => { return SubtractExpression(firstOp, secondOp); }},
            { OperationType.RightShiftExpression, (object firstOp, object secondOp) => { return RightShiftExpression(firstOp, secondOp); }},
            { OperationType.LeftShiftExpression, (object firstOp, object secondOp) => { return LeftShiftExpression(firstOp, secondOp); }},
            { OperationType.BitwiseNotExpression, (object firstOp, object secondOp) => { return BitwiseNotExpression(firstOp); }},
            { OperationType.LogicalAndExpression, (object firstOp, object secondOp) => { return LogicalAndExpression(firstOp, secondOp); }},
            { OperationType.LogicalOrExpression, (object firstOp, object secondOp) => { return LogicalOrExpression(firstOp, secondOp); }},
            { OperationType.ExclusiveOrExpression, (object firstOp, object secondOp) => { return ExclusiveOrExpression(firstOp, secondOp); }},
            { OperationType.BitwiseAndExpression, (object firstOp, object secondOp) => { return BitwiseAndExpression(firstOp, secondOp); }},
            { OperationType.BitwiseOrExpression, (object firstOp, object secondOp) => { return BitwiseOrExpression(firstOp, secondOp); }},
            { OperationType.LogicalNotExpression, (object firstOp, object secondOp) => { return LogicalNotExpression(firstOp); }},
            { OperationType.EqualsExpression, (object firstOp, object secondOp) => { return EqualsExpression(firstOp, secondOp); }},
            { OperationType.NotEqualsExpression, (object firstOp, object secondOp) => { return NotEqualsExpression(firstOp, secondOp); }},
            { OperationType.LessThanExpression, (object firstOp, object secondOp) => { return LessThanExpression(firstOp, secondOp); }},
            { OperationType.GreaterThanExpression, (object firstOp, object secondOp) => { return GreaterThanExpression(firstOp, secondOp); }},
            { OperationType.LessThanOrEqualExpression, (object firstOp, object secondOp) => { return LessThanOrEqualExpression(firstOp, secondOp); }},
            { OperationType.GreaterThanOrEqualExpression, (object firstOp, object secondOp) => { return GreaterThanOrEqualExpression(firstOp, secondOp); }},
            { OperationType.UnaryPlusExpression, (object firstOp, object secondOp) => { return UnaryPlusExpression(firstOp); }},
            { OperationType.UnaryMinusExpression, (object firstOp, object secondOp) => { return UnaryMinusExpression(firstOp); }}
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
            { typeof(UInt64), BasicTypes.TypeUInt64 },
            { typeof(String), BasicTypes.TypeString }
        };

        /// <summary>
        /// Converts value ​​to a IntPtr to IntPtr
        /// </summary>
        /// <param name="value">source value</param>
        /// <returns></returns>
        private static IntPtr valueToPtr(object value) 
        {
            if (value.GetType() == typeof(string))
                return Marshal.StringToBSTR(value as string);

            dynamic dynValue = value;
            byte[] bytes = BitConverter.GetBytes(dynValue);
            IntPtr ptr = Marshal.AllocCoTaskMem(bytes.Length);
            for (int i = 0; i < bytes.Length; i++)
            {
                Marshal.WriteByte(ptr, i, bytes[i]);
            }
            return ptr;
        }

        private static object ptrToValue(IntPtr ptr, int type) 
        {
            if ((BasicTypes)type == BasicTypes.TypeString)
            {
                if (ptr == IntPtr.Zero)
                    return String.Empty;
                else
                    return Marshal.PtrToStringBSTR(ptr);
            }

            var intValue = Marshal.ReadInt64(ptr);
            var bytesArray = BitConverter.GetBytes(intValue);
            return typesMap[(BasicTypes)type](bytesArray);
        }

        internal static RetCode CalculationDelegate(IntPtr firstOpPtr, int firstType, IntPtr secondOpPtr, int secondType, int operation, out int resultType, out IntPtr result, out IntPtr errorText)
        {
            resultType = 0;
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
            catch (System.Exception ex)
            {
                errorText = Marshal.StringToBSTR("error: " + ex.Message);
                return RetCode.Exception;
            }

            return RetCode.OK;
        }

        private static object AddExpression(dynamic first, dynamic second)
        {
            return first + second;
        }

        private static object SubtractExpression(dynamic first, dynamic second)
        {
            return first - second;
        }

        private static object MultiplyExpression(dynamic first, dynamic second)
        {
            return first * second;
        }

        private static object DivideExpression(dynamic first, dynamic second)
        {
            return first / second;
        }

        private static object ModuloExpression(dynamic first, dynamic second)
        {
            return first % second;
        }

        private static object RightShiftExpression(dynamic first, dynamic second) 
        {
            return first >> second;
        }

        private static object LeftShiftExpression(dynamic first, dynamic second)
        {
            return first << second;
        }

        private static object BitwiseNotExpression(dynamic first)
        {
            return ~first;
        }

        private static object ExclusiveOrExpression(dynamic first, dynamic second)
        {
            return first ^ second;
        }

        private static object BitwiseAndExpression(dynamic first, dynamic second)
        {
            return first & second;
        }

        private static object BitwiseOrExpression(dynamic first, dynamic second)
        {
            return first | second;
        }

        private static bool LogicalAndExpression(dynamic first, dynamic second)
        {
            return first && second;
        }

        private static bool LogicalOrExpression(dynamic first, dynamic second)
        {
            return first || second;
        }

        private static object LogicalNotExpression(dynamic first)
        {
            return !first;
        }

        private static bool EqualsExpression(dynamic first, dynamic second)
        {
            return first == second;
        }

        private static bool NotEqualsExpression(dynamic first, dynamic second) 
        {
            return first != second;
        }

        private static bool LessThanExpression(dynamic first, dynamic second)
        {
            return first < second;
        }

        private static bool GreaterThanExpression(dynamic first, dynamic second)
        {
            return first > second;
        }

        private static bool LessThanOrEqualExpression(dynamic first, dynamic second)
        {
            return first <= second;
        }

        private static bool GreaterThanOrEqualExpression(dynamic first, dynamic second)
        {
            return first >= second;
        }

        private static object UnaryPlusExpression(dynamic first)
        {
            return +first;
        }

        private static object UnaryMinusExpression(dynamic first)
        {
            return -first;
        }
    }
}
