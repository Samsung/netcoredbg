// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "tokenizer.h"

using std::string;

Tokenizer::Tokenizer(const std::string &str, const std::string &delimiters)
    : m_str(str), m_delimiters(delimiters), m_next(0)
{
    m_str.erase(m_str.find_last_not_of(m_delimiters) + 1);
}

bool Tokenizer::Next(std::string &token)
{
    token = "";

    if (m_next >= m_str.size())
        return false;

    enum {
        StateSpace,
        StateToken,
        StateQuotedToken,
        StateEscape
    } state = StateSpace;

    for (; m_next < m_str.size(); m_next++)
    {
        char c = m_str.at(m_next);
        switch(state)
        {
            case StateSpace:
                if (m_delimiters.find(c) != std::string::npos)
                    continue;
                if (!token.empty())
                    return true;
                state = c == '"' ? StateQuotedToken : StateToken;
                if (state != StateQuotedToken)
                    token +=c;
                break;
            case StateToken:
                if (m_delimiters.find(c) != std::string::npos)
                    state = StateSpace;
                else
                    token += c;
                break;
            case StateQuotedToken:
                if (c == '\\')
                    state = StateEscape;
                else if (c == '"')
                    state = StateSpace;
                else
                    token += c;
                break;
            case StateEscape:
                token += c;
                state = StateQuotedToken;
                break;
        }
    }
    return state != StateEscape || token.empty();
}

std::string Tokenizer::Remain() const
{
    return m_str.substr(m_next);
}
