// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

/// \file interop_unix.h  This file contains unix-specific functions for Interop class defined in interop.h

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#include <dirent.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <set>

#include "managed/interop.h"
#include "filesystem.h"

namespace netcoredbg
{

// This function searches *.dll files in specified directory and adds full paths to files
// to colon-separated list `tpaList'.
template <>
void InteropTraits<UnixPlatformTag>::AddFilesFromDirectoryToTpaList(const std::string &directory, std::string& tpaList)
{
    const char * const tpaExtensions[] = {
                ".ni.dll",      // Probe for .ni.dll first so that it's preferred if ni and il coexist in the same dir
                ".dll",
                ".ni.exe",
                ".exe",
                };

    DIR* dir = opendir(directory.c_str());
    if (dir == nullptr)
        return;

    std::set<std::string> addedAssemblies;

    // Walk the directory for each extension separately so that we first get files with .ni.dll extension,
    // then files with .dll extension, etc.
    for (size_t extIndex = 0; extIndex < sizeof(tpaExtensions) / sizeof(tpaExtensions[0]); extIndex++)
    {
        const char* ext = tpaExtensions[extIndex];
        int extLength = strlen(ext);

        struct dirent* entry;

        // For all entries in the directory
        while ((entry = readdir(dir)) != nullptr)
        {
            // We are interested in files only
            switch (entry->d_type)
            {
            case DT_REG:
                break;

            // Handle symlinks and file systems that do not support d_type
            case DT_LNK:
            case DT_UNKNOWN:
            {
                std::string fullFilename;

                fullFilename.append(directory);
                fullFilename += FileSystem::PathSeparator;
                fullFilename.append(entry->d_name);

                struct stat sb;
                if (stat(fullFilename.c_str(), &sb) == -1)
                    continue;

                if (!S_ISREG(sb.st_mode))
                    continue;
            }
            break;

            default:
                continue;
            }

            std::string filename(entry->d_name);

            // Check if the extension matches the one we are looking for
            int extPos = filename.length() - extLength;
            if ((extPos <= 0) || (filename.compare(extPos, extLength, ext) != 0))
            {
                continue;
            }

            std::string filenameWithoutExt(filename.substr(0, extPos));

            // Make sure if we have an assembly with multiple extensions present,
            // we insert only one version of it.
            if (addedAssemblies.find(filenameWithoutExt) == addedAssemblies.end())
            {
                addedAssemblies.insert(filenameWithoutExt);

                tpaList.append(directory);
                tpaList += FileSystem::PathSeparator;
                tpaList.append(filename);
                tpaList.append(":");
            }
        }

        // Rewind the directory stream to be able to iterate over it for the next extension
        rewinddir(dir);
    }

    closedir(dir);
}

// This function unsets `CORECLR_ENABLE_PROFILING' environment variable.
template <>
void InteropTraits<UnixPlatformTag>::UnsetCoreCLREnv()
{
    unsetenv("CORECLR_ENABLE_PROFILING");
}

#define ULONG_ERROR     (0xffffffffUL)
#define INTSAFE_E_ARITHMETIC_OVERFLOW       ((HRESULT)0x80070216L)  // 0x216 = 534 = ERROR_ARITHMETIC_OVERFLOW
#define UInt32x32To64(a, b) ((unsigned __int64)((ULONG)(a)) * (unsigned __int64)((ULONG)(b)))
#define WIN32_ALLOC_ALIGN (16 - 1)

__inline HRESULT ULongLongToULong(IN ULONGLONG ullOperand, OUT ULONG* pulResult)
{
    HRESULT hr = INTSAFE_E_ARITHMETIC_OVERFLOW;
    *pulResult = ULONG_ERROR;
    
    if (ullOperand <= ULONG_MAX)
    {
        *pulResult = (ULONG)ullOperand;
        hr = S_OK;
    }
    
    return hr;
}

__inline HRESULT ULongAdd(IN ULONG ulAugend, IN ULONG ulAddend, OUT ULONG* pulResult)
{
    HRESULT hr = INTSAFE_E_ARITHMETIC_OVERFLOW;
    *pulResult = ULONG_ERROR;

    if ((ulAugend + ulAddend) >= ulAugend)
    {
        *pulResult = (ulAugend + ulAddend);
        hr = S_OK;
    }
    
    return hr;
}

__inline HRESULT ULongMult(IN ULONG ulMultiplicand, IN ULONG ulMultiplier, OUT ULONG* pulResult)
{
    ULONGLONG ull64Result = UInt32x32To64(ulMultiplicand, ulMultiplier);
    
    return ULongLongToULong(ull64Result, pulResult);
}

inline HRESULT CbSysStringSize(ULONG cchSize, BOOL isByteLen, ULONG *result)
{
    if (result == NULL)
        return E_INVALIDARG;

    // +2 for the null terminator
    // + DWORD_PTR to store the byte length of the string
    int constant = sizeof(WCHAR) + sizeof(DWORD_PTR) + WIN32_ALLOC_ALIGN;

    if (isByteLen)
    {
        if (SUCCEEDED(ULongAdd(constant, cchSize, result)))
        {
            *result = *result & ~WIN32_ALLOC_ALIGN;
            return NOERROR;
        }
    }
    else
    {
        ULONG temp = 0; // should not use in-place addition in ULongAdd
        if (SUCCEEDED(ULongMult(cchSize, sizeof(WCHAR), &temp)) &
            SUCCEEDED(ULongAdd(temp, constant, result)))
        {
            *result = *result & ~WIN32_ALLOC_ALIGN;
            return NOERROR;
        }
    }
    return INTSAFE_E_ARITHMETIC_OVERFLOW;
}

// Allocates a new string, copies the specified number of characters from the passed string, and appends a null-terminating character.
template <>
BSTR InteropTraits<UnixPlatformTag>::SysAllocStringLen(const OLECHAR *psz, UINT len)
{
    BSTR bstr;
    DWORD cbTotal = 0;

    if (FAILED(CbSysStringSize(len, FALSE, &cbTotal)))
        return NULL;

    bstr = (OLECHAR *)malloc(cbTotal);

    if (bstr != NULL)
    {

#if defined(_WIN64)
        // NOTE: There are some apps which peek back 4 bytes to look at the size of the BSTR. So, in case of 64-bit code,
        // we need to ensure that the BSTR length can be found by looking one DWORD before the BSTR pointer. 
        *(DWORD_PTR *)bstr = (DWORD_PTR) 0;
        bstr = (BSTR) ((char *) bstr + sizeof (DWORD));
#endif
        *(DWORD FAR*)bstr = (DWORD)len * sizeof(OLECHAR);

        bstr = (BSTR) ((char*) bstr + sizeof(DWORD));

        if (psz != NULL)
            memcpy(bstr, psz, len * sizeof(OLECHAR));

        bstr[len] = '\0'; // always 0 terminate
    }

    return bstr;
}

// Deallocates a string allocated previously by SysAllocString, SysAllocStringByteLen, SysReAllocString, SysAllocStringLen, or SysReAllocStringLen.
template <>
void InteropTraits<UnixPlatformTag>::SysFreeString(BSTR bstrString)
{
    if (bstrString == NULL)
        return;
    free((BYTE *)bstrString-sizeof(DWORD_PTR));    
}

// Returns the length of a BSTR.
template <>
UINT InteropTraits<UnixPlatformTag>::SysStringLen(BSTR bstrString)
{
    if (bstrString == NULL)
        return 0;
    return (unsigned int)((((DWORD FAR*)bstrString)[-1]) / sizeof(OLECHAR));
}

// Allocates a block of task memory in the same way that IMalloc::Alloc does.
template <>
LPVOID InteropTraits<UnixPlatformTag>::CoTaskMemAlloc(size_t cb)
{
    return malloc(cb);
}

// Frees a block of task memory previously allocated through a call to the CoTaskMemAlloc or CoTaskMemRealloc function.
template <>
void InteropTraits<UnixPlatformTag>::CoTaskMemFree(LPVOID pt)
{
    free(pt);
}

}  // ::netcoredbg
#endif  // __unix__
