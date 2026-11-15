#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cvs
{
    // Just enough JSON to read Twitch's replies. Look-ups never throw: a missing key, a
    // value of the wrong type or an index past the end gives null, so a chain like
    // doc["payload"]["event"]["user_login"].Str() is safe on any input.
    class Json
    {
    public:
        enum class Type { Null, Bool, Number, String, Array, Object };

        // Nothing for malformed input, input nested more than 64 deep, or trailing junk.
        static std::optional<Json> Parse(std::string_view text);

        Type GetType() const { return m_type; }
        bool IsNull() const { return m_type == Type::Null; }
        const Json& operator[](std::string_view key) const;
        const Json& At(size_t index) const;
        size_t Size() const; // an array's elements or an object's members, otherwise 0

        std::string Str(const std::string& fallback = "") const;
        double Num(double fallback = 0) const;
        bool Bool(bool fallback = false) const;

    private:
        friend class JsonReader;
        Type m_type = Type::Null;
        bool m_bool = false;
        double m_number = 0;
        std::string m_string;
        std::vector<Json> m_items;       // an array's elements, or an object's values
        std::vector<std::string> m_keys; // an object's keys, matching m_items
    };
}
