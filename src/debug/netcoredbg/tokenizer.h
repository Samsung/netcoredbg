// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <string>

class Tokenizer
{
    std::string m_str;
    std::string m_delimiters;
    size_t m_next;
public:

    Tokenizer(const std::string &str, const std::string &delimiters = " \t\n\r");
    bool Next(std::string &token);
    std::string Remain() const;
};
