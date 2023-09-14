// Copyright (C) 2021 Samsung Electronics Co., Ltd.
// See the LICENSE file in the project root for more information.

#include <array>
#include <utility>
#include <type_traits>
#include <tuple>
#include "utils/string_view.h"
#include "utils/limits.h"

#ifndef LINE_MAX
#define LINE_MAX 2048
#endif

namespace netcoredbg
{

using Utility::string_view;

struct CLIHelperBase
{
protected:
    // This template defines type of the function which handles the command or the completion
    // defined by particular `Tag` type and particular `Binder` template...
    template <typename Tag, template <Tag> class Binder>
    using HandlerType = decltype(Binder<static_cast<Tag>(0)>::handler);

public:
    // `DispatchTable` is the template, which can be instantiated for some
    // particular `Tag` type (command or completion), particuar `Binder` template
    // (which resolves Tag values to functions), and number of tags.
    // Template instantiates to structure, which have `values[]` array,
    // where for each tag value (array index) there is a pointer to appropriate
    // handler function, which handles the command or the completion corresponding
    // to some particular tag.
    template <typename Tag, template <Tag> class Binder, Tag Count,
        typename = Utility::MakeSequence<size_t(Count)> >
    struct DispatchTable;

    // Instantiation of the template declared above.
    template <typename Tag, template <Tag> class Binder, Tag Count, size_t... Idx>
    struct DispatchTable<Tag, Binder, Count, Utility::Sequence<Idx...> >
    {
        static const HandlerType<Tag, Binder> values[];
    };

private:
    // Actual implementation of `complete_words` function.
    template <typename Func>
    static void complete_words(string_view prefix, Func&& func, const string_view *words, size_t size)
    {
        while (size)
        {
            if (words->size() >= prefix.size() && words->substr(0, prefix.size()) == prefix)
            {
	    	// create zero-terminated string
                char c_str[LINE_MAX];
                assert(words->size() < sizeof(c_str));
                words->copy(c_str, words->size());
                c_str[words->size()] = 0;
                func(c_str);
            }
            --size, ++words;
        }
    }

public:
    // Function which might be called from particular completion handler to provide
    // list of words to complete.
    template <size_t N, typename Func>
    static void complete_words(string_view prefix, Func&& func, const string_view (&words)[N])
    {
        complete_words(prefix, func, words, N);
    }
};


// Initialization of `values` array for DispatchTable class.
template <typename Tag, template <Tag> class Binder, Tag Count, size_t... Idx>
const CLIHelperBase::HandlerType<Tag, Binder>
CLIHelperBase::DispatchTable<Tag, Binder, Count, Utility::Sequence<Idx...> >::values[]
{ Binder<static_cast<Tag>(Idx)>::handler... };


// This class carries set of configurable typedefs for CLIHelper class...
template <
    typename AuxInfo,
    typename P_CommandTag, template <P_CommandTag> class CommandBinder,
    typename P_CompletionTag, template <P_CompletionTag> class CompletionBinder,
    size_t P_MaxAliases = 3, size_t P_MaxCompletions = 3
>
struct CLIHelperParams : CLIHelperBase
{
    static_assert(std::is_enum<P_CommandTag>::value, "invalid Params");
    static_assert(std::is_enum<P_CompletionTag>::value, "invalid Params");

    using CommandTag = P_CommandTag;
    using CompletionTag = P_CompletionTag;
    const constexpr static size_t MaxAliases = P_MaxAliases;
    const constexpr static size_t MaxCompletions = P_MaxCompletions;

    using CommandDispatchTable = DispatchTable<CommandTag, CommandBinder, CommandTag::CommandsCount>;
    using CompletionDispatchTable = DispatchTable<CompletionTag, CompletionBinder, CompletionTag::CompletionsCount>;

    // Data type which describes single command.
    struct CommandInfo
    {
        struct Completion
        {
            unsigned nword;
            CompletionTag ctag;
        };

        CommandTag tag;             // tag for the command
        const CommandInfo* sub;     // nested subcommands list
        std::array<Completion, MaxCompletions> completions; // possible completions
        std::array<string_view, MaxAliases> names;          // command aliases

        AuxInfo aux;                // auxilarry information (not used by this class)
    };
};


// Instantiation of this class allows to implement command line parser which allows to
// perform two basic things: 1) parse command and call appropriate handler function and
// 2) perform completion during command line editing...
template <typename Params>
class CLIHelper : public Params
{
    // following templates expands to true only if given type T is callable functor,
    // function pointer or function reference.
    template <typename T, typename = void>
    struct IsCallable : std::false_type {};

    template <typename T>
    struct IsCallable<T, Utility::Void<decltype(&T::operator())> > : std::true_type {};

    template <typename T>
    struct IsCallable<T, Utility::Void<typename std::enable_if<std::is_function<
        typename std::remove_reference<typename std::remove_pointer<T>::type>::type>::value>::type> >
            : std::true_type {};

    template <typename T>
    using CheckContract = IsCallable<typename std::decay<T>::type>;

public:
    using CommandTag = typename Params::CommandTag;
    using CompletionTag = typename Params::CompletionTag;

    using CommandInfo = typename Params::CommandInfo;

    using CommandDispatchTable = typename Params::CommandDispatchTable;
    using CompletionDispatchTable = typename Params::CompletionDispatchTable;

    // Class should be instantiated with the list/tree of commands descriptions.
    CLIHelper(const CommandInfo *commands_list) : commands_list(commands_list) {}

    // This function calls functor `func` for each possible completions for
    // currently edited command line `str` with cursor position `cur`.
    //
    // Functor might have any type, which has operator() and  which accepts value
    // of same type as CompletionBinder<CompletionTag>::handler has and somehow turn
    // this  value to function call to appropriate member function, which handles
    // particular type of completion. Typically, CompletionBinder<CompletionTag>::handler
    // returns pointer to member function, or other functor, which then should be called
    // (with provided this pointer, etc...) to get completions list.
    //
    // Function returns position in input string, starting from which completions
    // might replace text (till cursor position).
    //
    // Functor `func` should accept following arguments:
    //   * decltype(CompletionBinder<CompletionTag>::handler) -- currently unknown;
    //   * string_view -- string to complete (single word).
    //
    template <typename Func, typename = typename std::enable_if<CheckContract<Func>::value>::type>
    unsigned complete(string_view str, unsigned cur, Func&& func) const;

    // This function dispatches call to particular command handler,
    // if the command line (`str`) was successfully parsed.
    //
    // The functor itself might have any type, which has operator() and which accepts value
    // of same type which CommandBinder<CommandTag>::handler has and somehow turn this value
    // to function call to appropriate member function, which handles particular command.
    // Typically CommandBinder<CommandTag>::handler returns pointer to member function,
    // or other functor, which should be called (with provided this pointer, etc...)
    //
    // In the case, if command can't be parsed, functor will not be called, but
    // instead return value will contain non-empty string: part of the input string
    // which can't be parsed.
    //
    // Functor `func` should accept following arguments:
    //   * decltype(CommandBinder<CommandTag>::handler) -- currently unknown;
    //   * string_view -- while command line, as entered by user;
    //   * unsigned -- index in command line, at which unparsed command arguments starts.
    //
    template <typename Func, typename = typename std::enable_if<CheckContract<Func>::value>::type>
    string_view eval(string_view str, Func&& func) const;

private:
    // Symbols used to delimite words (commands parsing).
    static const char Delimiters[];

    // Top element of (sub)commands list tree.
    const CommandInfo *commands_list;

    // This function finds the specified command (`str`) in command descriptions
    // array (`ci`) and returns pointer to descriptor of mathing command. If no
    // command found, function returns NULL.
    // Function handles nested (multi-word) commands.
    static std::tuple<const CommandInfo*, size_t> find_command(const CommandInfo *ci, string_view str);

    // Function handles command arguments completions via CommandInfo::completions.
    // Arguments: ci -- descriptor of current command, str -- whole command line,
    // cur -- cursor position within command line, func -- callback function,
    // which will be called for each completion variant.
    template <typename Func>
    static unsigned complete_ext(const CommandInfo *ci, string_view str, unsigned cur, Func& func);

    // Function handles completions of subcommands.
    // Arguments: ci -- descriptor of current command, cmd -- incomplete subcommand,
    // func -- callback function, which will be called for each subcommands variant.
    template <typename Func>
    static bool complete_subcommand(const CommandInfo *ci, string_view cmd, Func& func);
};

// reserve memory / initialize static member
template <typename Params>
const char CLIHelper<Params>::Delimiters[] = "\r\n\v\t ";


template <typename Params>
std::tuple<const typename CLIHelper<Params>::CommandInfo*, size_t>
CLIHelper<Params>::find_command(const CLIHelper<Params>::CommandInfo *commands_list, string_view str)
{
    char const *cbegin = str.data();
    const CommandInfo *ci = commands_list;
    const CommandInfo *sub = nullptr;
    while (true)
    {
        // skip preceding spaces
        str.remove_prefix(std::min(str.size(), str.find_first_not_of(Delimiters)));

        // find word length
        string_view cmd = str.substr(0, str.find_first_of(Delimiters));
        str.remove_prefix(cmd.size());

        if (!cmd.size())
            return {sub, str.data() - cbegin};

        // find command description
        bool match = false;
        while (ci->tag != CommandTag::End)
        {
            for (const auto& name : ci->names)
            {
                if (!name.size()) break;

                if (name == cmd)
                {
                    match = true;
                    break;
                }
            }
            if (match) break;
            ++ci;
        }

        // (sub)command not found
        if (!match)
        {
            // previous subcommand with unknown argument
            if (sub)
                return {sub, cmd.data() - cbegin};

            // unknown command
            return {nullptr, str.data() - cbegin};
        }

        // full command
        if (!ci->sub)
            return {ci, str.data() - cbegin};

        // process subcommands
        sub = ci;
        ci = ci->sub;
    }
};


template <typename Params> template <typename Func>
unsigned CLIHelper<Params>::complete_ext(const CLIHelper<Params>::CommandInfo *ci,
    string_view str, unsigned cur, Func& func)
{
    // count words before the cursor
    unsigned nword = 0;
    string_view word;
    const char *const base = str.data();
    while (true)
    {
        // find beginning of the word
        str.remove_prefix(std::min(str.size(), str.find_first_not_of(Delimiters)));
        if (unsigned(str.data() - base) >= cur) break;

        nword++;
        word = str;

        // find end of the word
        str.remove_prefix(std::min(str.size(), str.find_first_of(Delimiters)));
    }

    // have non-empty prefix
    if (nword > 1)
        nword--;
    else
        word = str.substr(0, 0);

    // find completion for nword
    for (const auto& c : ci->completions)
    {
        if (c.nword == nword)
            func(CompletionDispatchTable::values[static_cast<size_t>(c.ctag)], word);
    }

    return unsigned(word.data() - base);
}


template <typename Params> template <typename Func>
bool CLIHelper<Params>::complete_subcommand(const CLIHelper<Params>::CommandInfo *ci,
    string_view cmd, Func& func)
{
    assert(ci);
    bool result = false;
    while (ci->tag != CommandTag::End)
    {
        for (const auto& name : ci->names)
        {
            if (!name.size()) break;

            if (cmd.size() <= name.size() && name.substr(0, cmd.size()) == cmd)
            {
                // create zero-terminated C-string
                char c_str[LINE_MAX];
                assert(name.size() < sizeof(c_str));
                name.copy(c_str, name.size());
                c_str[name.size()] = 0;
                func(CompletionDispatchTable::values[static_cast<size_t>(CompletionTag::Command)], c_str);
                result = true;
            }
        }

        ++ci;
    }
    return result;
}


template <typename Params> template <typename Func, typename>
unsigned CLIHelper<Params>::complete(string_view str, unsigned cur, Func&& func) const
{
    // command/arg prefix, before cursor position, without preceding spaces
    string_view prefix = str.substr(0, cur);
    prefix.remove_prefix(std::min(prefix.size(), prefix.find_first_not_of(Delimiters)));
    auto retval = unsigned(prefix.data() - str.data());

    // search for prefix preceding last word, at first search for space
    // back from cursor position
    size_t last_space = prefix.find_last_of(Delimiters);
    if (last_space == string_view::npos)
    {
        // no spaces at all -- completing first command
        complete_subcommand(commands_list, prefix, func);
        return retval;
    }

    // find ending of the word preceding last word
    size_t last_word = prefix.substr(0, last_space).find_last_not_of(Delimiters);
    assert (last_word != string_view::npos);

    // completing subcommand
    const CommandInfo *ci;
    size_t pos;
    std::tie(ci, pos) = find_command(commands_list, prefix.substr(0, last_word + 1));
    if (!ci)
        return retval; // no completions: invalid command

    // try to complete subcommand name
    if (ci->sub)
    {
        string_view next = prefix.substr(last_space + 1);
        if (complete_subcommand(ci->sub, next, func))
            return unsigned(next.data() - str.data());
    }

    return complete_ext(ci, str, cur, func);
}

template <typename Params> template <typename Func, typename>
string_view CLIHelper<Params>::eval(string_view str, Func&& func) const
{
    const CommandInfo *ci;
    size_t len;
    std::tie(ci, len) = find_command(commands_list, str);
    if (!ci)
    {
        if (str.find_first_not_of(Delimiters) == string_view::npos)
            return {};  // no command at all
        else
            return str.substr(0, len); // can't parse full command
    }

    func(CommandDispatchTable::values[static_cast<size_t>(ci->tag)], str, len);
    return {};
}

} // ::netcoredbg
