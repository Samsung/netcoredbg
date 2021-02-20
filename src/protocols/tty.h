// Copyright (C) 2021 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.
#pragma once
#include "utils/literal_string.h"

namespace netcoredbg
{

/// This namespace contains definitions of few terminal controlling codes.
/// Here presented only codes, which is compatible with both, typical linux terminal
/// emulator (xterm) and with windows console.
namespace tty
{
    using Utility::literal;

    // Miscellaneous...
    constexpr const auto bell        = literal("\007");
    constexpr const auto backspace   = literal("\010");
    constexpr const auto erase_line  = literal("\033[2K");

    // Graphic parameters (compatible with monochrome VT100).
    constexpr const auto reset       = literal("\033[0m"); // reset all graphic parameters
    constexpr const auto bold        = literal("\033[1m");
    constexpr const auto halfbright  = literal("\033[2m"); // rarely works (just ignored)
    constexpr const auto underscore  = literal("\033[4m");
    constexpr const auto blink       = literal("\033[5m");
    constexpr const auto reverse     = literal("\033[7m");

    // These codes not compatible with monochrome VT100 terminal emulator.
    constexpr const auto red         = literal("\033[31m");
    constexpr const auto green       = literal("\033[32m");
    constexpr const auto brown       = literal("\033[33m");
    constexpr const auto blue        = literal("\033[34m");
    constexpr const auto magenta     = literal("\033[35m");
    constexpr const auto cyan        = literal("\033[36m");

    // Note: white and black colors not included in the list above intentionally:
    // these colors shouldn't be used as foreground colors, as user selected
    // background is typically black, white or grey (so symbols might become invisible).
    //
    // Background setting codes excluded intentionally too -- `reverse` might be used instead.
}

} // ::netcoredbg
