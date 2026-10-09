#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <charconv>
#include <type_traits>

class ArgParseError : public std::runtime_error
{
public:
    explicit ArgParseError(const std::string &m) : std::runtime_error(m) {}
};

class ArgParser
{
public:
    struct OptionMeta
    {
        char short_name = '\0'; // like 'n' for -n
        std::string long_name;  // like "name" for --name
        std::string description;
    };

    ArgParser() = default;

    void add_option(const std::string &long_name, char short_name = '\0', const std::string &desc = {})
    {
        OptionMeta m;
        m.short_name = short_name;
        m.long_name = long_name;
        m.description = desc;
        if (!long_name.empty())
        {
            meta_by_long_[long_name] = m;
        }
        if (short_name != '\0')
        {
            meta_by_short_[short_name] = m;
        }
    }

    bool parse(int argc, char *argv[], std::string *err = nullptr)
    {
        values_.clear();
        positional_.clear();
        program_name_ = (argc > 0 ? std::string(argv[0]) : std::string());
        for (int i = 1; i < argc; ++i)
        {
            std::string a = argv[i];
            if (a.size() >= 1 && a[0] == '-')
            {
                // option
                if (a.size() >= 2 && a[1] == '-')
                {
                    // long form --name or --name=val
                    std::string body = a.substr(2);
                    auto eq = body.find('=');
                    if (eq != std::string::npos)
                    {
                        std::string key = body.substr(0, eq);
                        std::string val = body.substr(eq + 1);
                        set_value(key, val);
                    }
                    else
                    {
                        std::string key = body;
                        // lookahead for a value (if next exists and not start with '-')
                        if (i + 1 < argc)
                        {
                            std::string next = argv[i + 1];
                            if (!next.empty() && next[0] != '-')
                            {
                                set_value(key, next);
                                ++i;
                            }
                            else
                            {
                                // boolean flag -> true
                                set_value(key, "true");
                            }
                        }
                        else
                        {
                            // boolean flag
                            set_value(key, "true");
                        }
                    }
                }
                else
                {
                    // short form -n or -n=val  (does not support combined -abc)
                    std::string body = a.substr(1);
                    auto eq = body.find('=');
                    if (eq != std::string::npos)
                    {
                        std::string key = body.substr(0, eq);
                        std::string val = body.substr(eq + 1);
                        if (key.size() == 1)
                        {
                            set_value(std::string(1, key[0]), val, true);
                        }
                        else
                        {
                            // treat as long-ish short; still store with key
                            set_value(key, val, true);
                        }
                    }
                    else
                    {
                        if (body.size() == 1)
                        {
                            char s = body[0];
                            // lookahead for value
                            if (i + 1 < argc)
                            {
                                std::string next = argv[i + 1];
                                if (!next.empty() && next[0] != '-')
                                {
                                    set_value(std::string(1, s), next, true);
                                    ++i;
                                }
                                else
                                {
                                    set_value(std::string(1, s), "true", true);
                                }
                            }
                            else
                            {
                                set_value(std::string(1, s), "true", true);
                            }
                        }
                        else
                        {
                            // treat "-name" as long name without dashes
                            set_value(body, "true");
                        }
                    }
                }
            }
            else
            {
                // positional argument
                positional_.push_back(a);
            }
        }
        return true;
    }

    bool has(const std::string &name) const
    {
        return values_.find(name) != values_.end();
    }
    bool has(char short_name) const
    {
        std::string key(1, short_name);
        return values_.find(key) != values_.end();
    }

    const std::vector<std::string> &positional() const { return positional_; }
    std::string program_name() const { return program_name_; }

    std::optional<std::string> get_raw(const std::string &name) const
    {
        auto it = values_.find(name);
        if (it != values_.end())
            return it->second;
        // also try short name registered to a long meta
        auto metaIt = meta_by_long_.find(name);
        if (metaIt != meta_by_long_.end())
        {
            char s = metaIt->second.short_name;
            if (s != '\0')
            {
                auto it2 = values_.find(std::string(1, s));
                if (it2 != values_.end())
                    return it2->second;
            }
        }
        return std::nullopt;
    }

    template <typename T>
    T get(const std::string &name) const
    {
        auto raw = get_raw(name);
        if (!raw)
            throw ArgParseError("missing option: " + name);
        return convert<T>(*raw);
    }

    template <typename T>
    T get_or(const std::string &name, T default_value) const
    {
        auto raw = get_raw(name);
        if (!raw)
            return default_value;
        try
        {
            return convert<T>(*raw);
        }
        catch (...)
        {
            return default_value;
        }
    }

    template <typename T>
    T get(char short_name) const
    {
        auto it = values_.find(std::string(1, short_name));
        if (it == values_.end())
            throw ArgParseError(std::string("missing option: ") + short_name);
        return convert<T>(it->second);
    }

    std::string last_error() const { return last_error_; }

private:
    std::unordered_map<std::string, OptionMeta> meta_by_long_;
    std::unordered_map<char, OptionMeta> meta_by_short_;

    std::unordered_map<std::string, std::string> values_; // stores original keys and short char keys as strings
    std::vector<std::string> positional_;
    std::string program_name_;
    std::string last_error_;

    void set_value(const std::string &key, const std::string &val, bool is_short = false)
    {
        if (is_short && key.size() == 1)
        {
            values_[key] = val;
        }
        else
        {
            // by default store by long name if registered, otherwise by key
            if (!is_short)
            {
                if (meta_by_long_.find(key) != meta_by_long_.end())
                {
                    values_[key] = val;
                }
                else
                {
                    // maybe there's a short meta mapping with same string? store raw key
                    values_[key] = val;
                }
            }
            else
            {
                values_[key] = val;
            }
        }
    }

    // conversion helpers
    template <typename T>
    static T convert(const std::string &s)
    {
        if constexpr (std::is_same<T, std::string>::value)
        {
            return s;
        }
        else if constexpr (std::is_same<T, bool>::value)
        {
            // accept "1", "true", "yes", "on"
            if (s == "1")
                return true;
            std::string lower = s;
            for (auto &c : lower)
                c = (char)std::tolower(c);
            return (lower == "true" || lower == "yes" || lower == "on");
        }
        else if constexpr (std::is_integral<T>::value && !std::is_same<T, bool>::value)
        {
            // use from_chars for integer types
            T val{};
            auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
            if (ec != std::errc())
                throw ArgParseError("invalid integer: " + s);
            return val;
        }
        else if constexpr (std::is_floating_point<T>::value)
        {
            // fallback to stod/stof
            if constexpr (std::is_same<T, double>::value)
            {
                try
                {
                    return std::stod(s);
                }
                catch (...)
                {
                    throw ArgParseError("invalid double: " + s);
                }
            }
            else if constexpr (std::is_same<T, float>::value)
            {
                try
                {
                    return std::stof(s);
                }
                catch (...)
                {
                    throw ArgParseError("invalid float: " + s);
                }
            }
            else
            {
                // long double etc.
                try
                {
                    return static_cast<T>(std::stold(s));
                }
                catch (...)
                {
                    throw ArgParseError("invalid float: " + s);
                }
            }
        }
        else
        {
            static_assert(!sizeof(T *), "unsupported type for ArgParser::get<T>()");
        }
    }
};
