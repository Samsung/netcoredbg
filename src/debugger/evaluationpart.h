// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "cor.h"

#include <string>
#include <vector>

namespace netcoredbg
{

struct EvaluationPart
{
    std::string name;
    std::vector<ULONG32> indexes;

    EvaluationPart() = default;
    EvaluationPart(const std::string &Name) :
        name(Name),
        indexes()
    {}
    EvaluationPart(std::vector<ULONG32> &Indexes) :
        name(),
        indexes(Indexes)
    {}
};

} // namespace netcoredbg
