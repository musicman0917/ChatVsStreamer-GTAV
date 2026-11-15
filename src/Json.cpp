#include "Json.h"

#include <cmath>
#include <cstdlib>

namespace cvs
{
    namespace
    {
        const Json& Null()
        {
            static const Json null;
            return null;
        }

        void AppendUtf8(std::string& out, unsigned cp)
        {
            if (cp < 0x80) out.push_back((char)cp);
            else if (cp < 0x800)
            {
                out.push_back((char)(0xC0 | (cp >> 6)));
                out.push_back((char)(0x80 | (cp & 0x3F)));
            }
            else if (cp < 0x10000)
            {
                out.push_back((char)(0xE0 | (cp >> 12)));
                out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back((char)(0x80 | (cp & 0x3F)));
            }
            else
            {
                out.push_back((char)(0xF0 | (cp >> 18)));
                out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back((char)(0x80 | (cp & 0x3F)));
            }
        }
    }

    class JsonReader
    {
    public:
        explicit JsonReader(std::string_view text) : m_text(text) {}

        bool ReadDocument(Json& out)
        {
            if (!ReadValue(out, 0)) return false;
            SkipSpace();
            return m_pos == m_text.size();
        }

    private:
        // Deep enough for anything Twitch sends, shallow enough that hostile input can't
        // overflow the stack.
        static constexpr int kMaxDepth = 64;

        bool More() const { return m_pos < m_text.size(); }
        char Peek() const { return m_text[m_pos]; }

        void SkipSpace()
        {
            while (More() && (Peek() == ' ' || Peek() == '\t' || Peek() == '\n' || Peek() == '\r')) ++m_pos;
        }

        bool Literal(std::string_view word)
        {
            if (m_text.substr(m_pos, word.size()) != word) return false;
            m_pos += word.size();
            return true;
        }

        bool ReadValue(Json& out, int depth)
        {
            if (depth > kMaxDepth) return false;
            SkipSpace();
            if (!More()) return false;
            switch (Peek())
            {
            case '{': return ReadObject(out, depth);
            case '[': return ReadArray(out, depth);
            case '"': out.m_type = Json::Type::String; return ReadString(out.m_string);
            case 't': out.m_type = Json::Type::Bool; out.m_bool = true; return Literal("true");
            case 'f': out.m_type = Json::Type::Bool; out.m_bool = false; return Literal("false");
            case 'n': out.m_type = Json::Type::Null; return Literal("null");
            default: return ReadNumber(out);
            }
        }

        bool ReadObject(Json& out, int depth)
        {
            out.m_type = Json::Type::Object;
            ++m_pos;
            SkipSpace();
            if (More() && Peek() == '}') { ++m_pos; return true; }
            while (true)
            {
                SkipSpace();
                std::string key;
                if (!More() || Peek() != '"' || !ReadString(key)) return false;
                SkipSpace();
                if (!More() || Peek() != ':') return false;
                ++m_pos;
                Json value;
                if (!ReadValue(value, depth + 1)) return false;
                out.m_keys.push_back(std::move(key));
                out.m_items.push_back(std::move(value));
                SkipSpace();
                if (!More()) return false;
                if (Peek() == ',') { ++m_pos; continue; }
                if (Peek() == '}') { ++m_pos; return true; }
                return false;
            }
        }

        bool ReadArray(Json& out, int depth)
        {
            out.m_type = Json::Type::Array;
            ++m_pos;
            SkipSpace();
            if (More() && Peek() == ']') { ++m_pos; return true; }
            while (true)
            {
                Json value;
                if (!ReadValue(value, depth + 1)) return false;
                out.m_items.push_back(std::move(value));
                SkipSpace();
                if (!More()) return false;
                if (Peek() == ',') { ++m_pos; continue; }
                if (Peek() == ']') { ++m_pos; return true; }
                return false;
            }
        }

        bool ReadHex4(unsigned& out)
        {
            if (m_pos + 4 > m_text.size()) return false;
            out = 0;
            for (int i = 0; i < 4; ++i)
            {
                const char c = m_text[m_pos++];
                out <<= 4;
                if (c >= '0' && c <= '9') out |= (unsigned)(c - '0');
                else if (c >= 'a' && c <= 'f') out |= (unsigned)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') out |= (unsigned)(c - 'A' + 10);
                else return false;
            }
            return true;
        }

        bool ReadString(std::string& out)
        {
            ++m_pos; // the opening quote
            while (More())
            {
                const char c = m_text[m_pos++];
                if (c == '"') return true;
                if ((unsigned char)c < 0x20) return false; // JSON doesn't allow raw control characters
                if (c != '\\') { out.push_back(c); continue; }
                if (!More()) return false;
                switch (m_text[m_pos++])
                {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u':
                {
                    unsigned cp;
                    if (!ReadHex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF)
                    {
                        // Outside the basic plane (most emoji), a character comes as two halves.
                        unsigned low;
                        if (m_text.substr(m_pos, 2) != "\\u") return false;
                        m_pos += 2;
                        if (!ReadHex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    }
                    else if (cp >= 0xDC00 && cp <= 0xDFFF) return false;
                    AppendUtf8(out, cp);
                    break;
                }
                default: return false;
                }
            }
            return false; // no closing quote
        }

        bool ReadNumber(Json& out)
        {
            const size_t start = m_pos;
            const auto digits = [this]
            {
                const size_t from = m_pos;
                while (More() && Peek() >= '0' && Peek() <= '9') ++m_pos;
                return m_pos > from;
            };
            if (More() && Peek() == '-') ++m_pos;
            if (!digits()) return false;
            if (More() && Peek() == '.')
            {
                ++m_pos;
                if (!digits()) return false;
            }
            if (More() && (Peek() == 'e' || Peek() == 'E'))
            {
                ++m_pos;
                if (More() && (Peek() == '+' || Peek() == '-')) ++m_pos;
                if (!digits()) return false;
            }
            const std::string token(m_text.substr(start, m_pos - start));
            out.m_type = Json::Type::Number;
            out.m_number = strtod(token.c_str(), nullptr);
            return std::isfinite(out.m_number);
        }

        std::string_view m_text;
        size_t m_pos = 0;
    };

    std::optional<Json> Json::Parse(std::string_view text)
    {
        Json doc;
        JsonReader reader(text);
        if (!reader.ReadDocument(doc)) return std::nullopt;
        return doc;
    }

    const Json& Json::operator[](std::string_view key) const
    {
        if (m_type != Type::Object) return Null();
        for (size_t i = 0; i < m_keys.size(); ++i)
            if (m_keys[i] == key) return m_items[i];
        return Null();
    }

    const Json& Json::At(size_t index) const
    {
        return m_type == Type::Array && index < m_items.size() ? m_items[index] : Null();
    }

    size_t Json::Size() const
    {
        return m_type == Type::Array || m_type == Type::Object ? m_items.size() : 0;
    }

    std::string Json::Str(const std::string& fallback) const { return m_type == Type::String ? m_string : fallback; }
    double Json::Num(double fallback) const { return m_type == Type::Number ? m_number : fallback; }
    bool Json::Bool(bool fallback) const { return m_type == Type::Bool ? m_bool : fallback; }
}
