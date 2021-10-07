// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using System;
using System.Runtime.InteropServices;

namespace NetCoreDbg
{
    enum RetCode : int
    {
        OK = 0,
        Fail = 1,
        Exception = 2
    }

    public class Utils
    {
        internal static RetCode StringToUpper([MarshalAs(UnmanagedType.LPWStr)] string srcString, out IntPtr dstString)
        {
            dstString = IntPtr.Zero;

            try
            {
                dstString = Marshal.StringToBSTR(srcString.ToUpper());
            }
            catch
            {
                return RetCode.Exception;
            }

            return RetCode.OK;
        }

        internal static IntPtr SysAllocStringLen(int size)
        {
            string empty = new String('\0', size);
            return Marshal.StringToBSTR(empty);
        }

        internal static void SysFreeString(IntPtr ptr)
        {
            Marshal.FreeBSTR(ptr);
        }

        internal static IntPtr CoTaskMemAlloc(int size)
        {
            return Marshal.AllocCoTaskMem(size);
        }

        internal static void CoTaskMemFree(IntPtr ptr)
        {
            Marshal.FreeCoTaskMem(ptr);
        }
    }
}
