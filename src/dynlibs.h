// Copyright (C) 2020 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

/// \file dynlibs.h  This file contains declaration required to work
/// with dynamically loading libraries.

#pragma once
#include "string_view.h"

namespace netcoredbg
{
    using Utility::string_view;

    struct DLHandleRef;

    /// Opaque type representing loaded dynamic library handle.
    typedef DLHandleRef* DLHandle;

    /// This functon load specified library and returns handle (which then
    /// can be passed to DLSym and DLCLose functions).
    /// In case of error function returns NULL.
    DLHandle DLOpen(string_view path);

    /// This function resolves symbol address within library specified by handle,
    /// and returns it's address, in case of error function returns NULL.
    void* DLSym(DLHandle handle, string_view symbol);

    /// This function unloads previously loadded library, specified by handle.
    /// In case of error this function returns `false'.
    bool DLClose(DLHandle handle);

}  // ::netcoredbg
