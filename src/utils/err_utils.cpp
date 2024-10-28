// Copyright (C) 2024 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

#include <stddef.h>
#include <string.h>

// Same as strerror_r(3) for GNU-specific strerror_r() but for all platforms (Linux, not GNU Linux (musl), Windows, Mac):
// The GNU-specific strerror_r() returns a pointer to a string containing the error message.
// This may be either a pointer to a string that the function stores in buf, or a pointer
// to some (immutable) static string (in which case buf is unused).
const char *ErrGetStr(int err_code, char buf[], size_t buf_size)
{
#ifdef WIN32
    static const char errStr[] = "Unknown error";
    bool haveErrorStr = strerror_s(buf, buf_size, err_code) == 0;
    return haveErrorStr ? buf : errStr;
#else

#ifdef _GNU_SOURCE
    return strerror_r(err_code, buf, buf_size);
#else
    static const char errStr[] = "Unknown error";
    bool haveErrorStr = strerror_r(err_code, buf, buf_size) == 0;
    return haveErrorStr ? buf : errStr;
#endif //_GNU_SOURCE

#endif //WIN32
}
