// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

// \!file limits.h  This file includes system limits.h file and adds definition of LINE_MAX.

#pragma once

// avoid bug with limit.h and clang-6.x
#ifdef __linux__
#include <linux/limits.h>
#endif

// include system <limits.h> file
#include <../include/limits.h>

// define LINX_MAX for windows
#ifndef LINE_MAX
#define LINE_MAX (2048)
#endif

