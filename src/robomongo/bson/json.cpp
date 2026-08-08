// JSON parsing (Extended JSON v2 canonical/relaxed + legacy v1) and
// serialization for robobson. The parser is order-preserving, which QJson
// is not - field order matters in the document views.

#include "robomongo/bson/bson.h"

#include <cctype>
#include <cstdlib>
#include <stdexcept>

namespace mongo {

    using detail::BSONValue;
    using detail::BSONValuePtr;

    namespace {

        long long parseIsoDate(const std::string &iso);
        long long numberOf(const BSONValuePtr &v);

        // -------------------------------------------------------------
        // JSON tokenizer/parser producing a BSONValue tree where numbers
        // are tagged NumberDouble/NumberInt/NumberLong by shape. In
        // relaxed mode also accepts mongo shell notation: unquoted keys,
        // single quotes, ObjectId()/ISODate()/... constructors, regex
        // literals and trailing commas.
        // -------------------------------------------------------------

        class JsonParser {
        public:
            explicit JsonParser(const std::string &text, bool relaxed = false)
                : _s(text), _relaxed(relaxed) {}

            BSONValuePtr parse() {
                skipWs();
                BSONValuePtr v = parseValue();
                skipWs();
                if (_pos != _s.size())
                    fail("trailing characters after JSON value");
                return v;
            }

            /** Parses one value and stops; caller reads position() for the rest */
            BSONValuePtr parseOne() {
                skipWs();
                BSONValuePtr v = parseValue();
                skipWs();
                return v;
            }

            size_t position() const { return _pos; }

        private:
            [[noreturn]] void fail(const std::string &msg) {
                throw shelljson::ParseMsgAssertionException(
                    msg, static_cast<int>(_pos));
            }

            void skipWs() {
                while (_pos < _s.size() &&
                       (_s[_pos] == ' ' || _s[_pos] == '\t' || _s[_pos] == '\n' ||
                        _s[_pos] == '\r'))
                    ++_pos;
            }

            char peek() {
                if (_pos >= _s.size()) fail("unexpected end of input");
                return _s[_pos];
            }

            void expect(char c) {
                if (peek() != c) fail(std::string("expected '") + c + "'");
                ++_pos;
            }

            BSONValuePtr parseValue() {
                const char c = peek();
                if (c == '{') return parseObject();
                if (c == '[') return parseArray();
                if (c == '"' || (_relaxed && c == '\'')) return parseStringValue();
                if (_relaxed && (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$'))
                    return parseIdentifierValue();
                if (!_relaxed && (c == 't' || c == 'f')) return parseBool();
                if (!_relaxed && c == 'n') return parseNull();
                if (_relaxed && c == '/') return parseRegexLiteral();
                return parseNumber();
            }

            BSONValuePtr parseObject() {
                expect('{');
                auto obj = BSONValue::makeObject();
                skipWs();
                if (peek() == '}') { ++_pos; return obj; }
                while (true) {
                    skipWs();
                    std::string key = parseKey();
                    skipWs();
                    expect(':');
                    skipWs();
                    obj->fields.emplace_back(key, parseValue());
                    skipWs();
                    if (peek() == ',') {
                        ++_pos;
                        if (_relaxed) {  // tolerate trailing comma
                            skipWs();
                            if (peek() == '}') { ++_pos; break; }
                        }
                        continue;
                    }
                    expect('}');
                    break;
                }
                return obj;
            }

            std::string parseKey() {
                const char c = peek();
                if (c == '"' || (_relaxed && c == '\''))
                    return parseString();
                if (!_relaxed)
                    fail("expected quoted field name");
                // Unquoted identifier key (shell notation)
                std::string key;
                while (_pos < _s.size()) {
                    const char k = _s[_pos];
                    if (std::isalnum(static_cast<unsigned char>(k)) || k == '_' || k == '$' ||
                        k == '.') {
                        key += k;
                        ++_pos;
                    } else {
                        break;
                    }
                }
                if (key.empty())
                    fail("expected field name");
                return key;
            }

            BSONValuePtr parseArray() {
                expect('[');
                auto arr = BSONValue::makeArray();
                skipWs();
                if (peek() == ']') { ++_pos; return arr; }
                int index = 0;
                while (true) {
                    skipWs();
                    arr->fields.emplace_back(std::to_string(index++), parseValue());
                    skipWs();
                    if (peek() == ',') { ++_pos; continue; }
                    expect(']');
                    break;
                }
                return arr;
            }

            std::string parseString() {
                const char quote = peek();
                if (quote != '"' && !(_relaxed && quote == '\''))
                    fail("expected string");
                ++_pos;
                std::string out;
                while (true) {
                    if (_pos >= _s.size()) fail("unterminated string");
                    char c = _s[_pos++];
                    if (c == quote) break;
                    if (c == '\\') {
                        if (_pos >= _s.size()) fail("bad escape");
                        char e = _s[_pos++];
                        switch (e) {
                            case '"': out += '"'; break;
                            case '\\': out += '\\'; break;
                            case '/': out += '/'; break;
                            case 'b': out += '\b'; break;
                            case 'f': out += '\f'; break;
                            case 'n': out += '\n'; break;
                            case 'r': out += '\r'; break;
                            case 't': out += '\t'; break;
                            case 'u': {
                                if (_pos + 4 > _s.size()) fail("bad \\u escape");
                                unsigned code = static_cast<unsigned>(
                                    std::strtoul(_s.substr(_pos, 4).c_str(), nullptr, 16));
                                _pos += 4;
                                // Encode code point as UTF-8 (surrogate pairs for
                                // non-BMP characters)
                                if (code >= 0xD800 && code <= 0xDBFF && _pos + 6 <= _s.size() &&
                                    _s[_pos] == '\\' && _s[_pos + 1] == 'u') {
                                    unsigned low = static_cast<unsigned>(std::strtoul(
                                        _s.substr(_pos + 2, 4).c_str(), nullptr, 16));
                                    if (low >= 0xDC00 && low <= 0xDFFF) {
                                        _pos += 6;
                                        unsigned cp = 0x10000 +
                                            ((code - 0xD800) << 10) + (low - 0xDC00);
                                        out += static_cast<char>(0xF0 | (cp >> 18));
                                        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                                        out += static_cast<char>(0x80 | (cp & 0x3F));
                                        break;
                                    }
                                }
                                if (code < 0x80) {
                                    out += static_cast<char>(code);
                                } else if (code < 0x800) {
                                    out += static_cast<char>(0xC0 | (code >> 6));
                                    out += static_cast<char>(0x80 | (code & 0x3F));
                                } else {
                                    out += static_cast<char>(0xE0 | (code >> 12));
                                    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                                    out += static_cast<char>(0x80 | (code & 0x3F));
                                }
                                break;
                            }
                            default: fail("unknown escape");
                        }
                    } else {
                        out += c;
                    }
                }
                return out;
            }

            BSONValuePtr parseStringValue() {
                auto v = std::make_shared<BSONValue>();
                v->type = mongo::String;
                v->str = parseString();
                return v;
            }

            BSONValuePtr parseBool() {
                auto v = std::make_shared<BSONValue>();
                v->type = mongo::Bool;
                if (_s.compare(_pos, 4, "true") == 0) {
                    v->boolean = true;
                    _pos += 4;
                } else if (_s.compare(_pos, 5, "false") == 0) {
                    v->boolean = false;
                    _pos += 5;
                } else {
                    fail("bad literal");
                }
                return v;
            }

            BSONValuePtr parseNull() {
                auto v = std::make_shared<BSONValue>();
                if (_s.compare(_pos, 4, "null") == 0) {
                    v->type = jstNULL;
                    _pos += 4;
                } else {
                    fail("bad literal");
                }
                return v;
            }

            /** Regex literal: /pattern/flags */
            BSONValuePtr parseRegexLiteral() {
                expect('/');
                std::string pattern;
                while (_pos < _s.size() && _s[_pos] != '/') {
                    if (_s[_pos] == '\\' && _pos + 1 < _s.size())
                        pattern += _s[_pos++];
                    pattern += _s[_pos++];
                }
                expect('/');
                std::string flags;
                while (_pos < _s.size() &&
                       std::isalpha(static_cast<unsigned char>(_s[_pos])))
                    flags += _s[_pos++];
                auto v = std::make_shared<BSONValue>();
                v->type = RegEx;
                v->str = pattern;
                v->str2 = flags;
                return v;
            }

            std::string parseIdentifier() {
                std::string id;
                while (_pos < _s.size()) {
                    const char c = _s[_pos];
                    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
                        id += c;
                        ++_pos;
                    } else {
                        break;
                    }
                }
                return id;
            }

            /** Arguments of a shell constructor call: ( v1, v2, ... ) */
            std::vector<BSONValuePtr> parseCallArgs() {
                std::vector<BSONValuePtr> args;
                skipWs();
                expect('(');
                skipWs();
                if (peek() == ')') { ++_pos; return args; }
                while (true) {
                    skipWs();
                    args.push_back(parseValue());
                    skipWs();
                    if (peek() == ',') { ++_pos; continue; }
                    expect(')');
                    break;
                }
                return args;
            }

            static std::string argString(const std::vector<BSONValuePtr> &args, size_t i) {
                if (i >= args.size() || !args[i]) return std::string();
                const auto &v = *args[i];
                if (v.type == mongo::String) return v.str;
                if (v.type == NumberInt) return std::to_string(v.numberInt);
                if (v.type == NumberLong) return std::to_string(v.numberLong);
                if (v.type == NumberDouble) return std::to_string(v.numberDouble);
                return std::string();
            }

            static long long argNumber(const std::vector<BSONValuePtr> &args, size_t i) {
                if (i >= args.size() || !args[i]) return 0;
                return numberOf(args[i]);
            }

            /** Shell notation values: true/false/null/undefined, ObjectId(...), ... */
            BSONValuePtr parseIdentifierValue() {
                const size_t start = _pos;
                std::string id = parseIdentifier();

                if (id == "new") {  // new Date(...), new ObjectId(...)
                    skipWs();
                    id = parseIdentifier();
                }

                auto v = std::make_shared<BSONValue>();

                if (id == "true" || id == "false") {
                    v->type = mongo::Bool;
                    v->boolean = (id == "true");
                    return v;
                }
                if (id == "null") { v->type = jstNULL; return v; }
                if (id == "undefined") { v->type = Undefined; return v; }
                if (id == "MinKey") { v->type = MinKey; maybeSkipCall(); return v; }
                if (id == "MaxKey") { v->type = MaxKey; maybeSkipCall(); return v; }
                if (id == "NaN") { v->type = NumberDouble; v->numberDouble = NAN; return v; }
                if (id == "Infinity") { v->type = NumberDouble; v->numberDouble = HUGE_VAL; return v; }

                if (id == "ObjectId") {
                    v->type = jstOID;
                    v->oid = OID(argString(parseCallArgs(), 0));
                    return v;
                }
                if (id == "ISODate" || id == "Date") {
                    auto args = parseCallArgs();
                    v->type = mongo::Date;
                    if (!args.empty() && args[0]->type == mongo::String)
                        v->date = Date_t::fromMillisSinceEpoch(parseIsoDate(args[0]->str));
                    else
                        v->date = Date_t::fromMillisSinceEpoch(argNumber(args, 0));
                    return v;
                }
                if (id == "NumberLong") {
                    auto args = parseCallArgs();
                    v->type = NumberLong;
                    v->numberLong = args.empty() ? 0 :
                        std::strtoll(argString(args, 0).c_str(), nullptr, 10);
                    return v;
                }
                if (id == "NumberInt") {
                    v->type = NumberInt;
                    v->numberInt = static_cast<int>(argNumber(parseCallArgs(), 0));
                    return v;
                }
                if (id == "NumberDecimal") {
                    v->type = NumberDecimal;
                    v->str = argString(parseCallArgs(), 0);
                    return v;
                }
                if (id == "Timestamp") {
                    auto args = parseCallArgs();
                    v->type = bsonTimestamp;
                    v->timestamp = Timestamp(static_cast<unsigned>(argNumber(args, 0)),
                                             static_cast<unsigned>(argNumber(args, 1)));
                    return v;
                }
                if (id == "BinData") {
                    auto args = parseCallArgs();
                    v->type = BinData;
                    v->binSubType = static_cast<BinDataType>(argNumber(args, 0));
                    v->binary = base64::decode(argString(args, 1));
                    return v;
                }
                if (id == "UUID" || id == "LUUID" || id == "JUUID" || id == "NUUID" ||
                    id == "PYUUID") {
                    auto args = parseCallArgs();
                    v->type = BinData;
                    v->binSubType = (id == "UUID") ? newUUID : bdtUUID;
                    std::string hex = argString(args, 0);
                    hex.erase(std::remove_if(hex.begin(), hex.end(),
                                  [](char c) { return c == '-' || c == '{' || c == '}'; }),
                              hex.end());
                    for (size_t i = 0; i + 1 < hex.size(); i += 2)
                        v->binary += static_cast<char>(
                            std::strtol(hex.substr(i, 2).c_str(), nullptr, 16));
                    return v;
                }
                if (id == "DBRef") {
                    auto args = parseCallArgs();
                    v->type = DBRef;
                    v->str = argString(args, 0);
                    v->str2 = argString(args, 1);
                    return v;
                }
                if (id == "Code") {
                    v->type = Code;
                    v->str = argString(parseCallArgs(), 0);
                    return v;
                }
                if (id == "RegExp") {
                    auto args = parseCallArgs();
                    v->type = RegEx;
                    v->str = argString(args, 0);
                    v->str2 = argString(args, 1);
                    return v;
                }

                _pos = start;
                fail("unknown identifier '" + id + "'");
            }

            /** MinKey/MaxKey may be written with or without () */
            void maybeSkipCall() {
                skipWs();
                if (_pos < _s.size() && _s[_pos] == '(') {
                    parseCallArgs();
                }
            }

            BSONValuePtr parseNumber() {
                size_t start = _pos;
                if (peek() == '-') ++_pos;
                bool isDouble = false;
                while (_pos < _s.size()) {
                    char c = _s[_pos];
                    if (std::isdigit(static_cast<unsigned char>(c))) { ++_pos; continue; }
                    if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                        isDouble = true;
                        ++_pos;
                        continue;
                    }
                    break;
                }
                if (_pos == start) fail("invalid number");
                std::string text = _s.substr(start, _pos - start);
                auto v = std::make_shared<BSONValue>();
                if (isDouble) {
                    v->type = NumberDouble;
                    v->numberDouble = std::strtod(text.c_str(), nullptr);
                } else {
                    long long ll = std::strtoll(text.c_str(), nullptr, 10);
                    if (ll >= INT32_MIN && ll <= INT32_MAX) {
                        v->type = NumberInt;
                        v->numberInt = static_cast<int>(ll);
                    } else {
                        v->type = NumberLong;
                        v->numberLong = ll;
                    }
                }
                return v;
            }

            const std::string &_s;
            size_t _pos = 0;
            bool _relaxed = false;
        };

        // -------------------------------------------------------------
        // Extended JSON recognition: rewrite {$oid: ...}-style wrapper
        // objects into typed nodes. Handles v2 canonical/relaxed plus
        // common legacy v1 spellings.
        // -------------------------------------------------------------

        bool isField(const BSONValuePtr &obj, size_t i, const char *name) {
            return i < obj->fields.size() && obj->fields[i].first == name;
        }

        BSONValuePtr field(const BSONValuePtr &obj, const char *name) {
            for (const auto &f : obj->fields)
                if (f.first == name) return f.second;
            return nullptr;
        }

        long long numberOf(const BSONValuePtr &v) {
            if (!v) return 0;
            switch (v->type) {
                case NumberInt: return v->numberInt;
                case NumberLong: return v->numberLong;
                case NumberDouble: return static_cast<long long>(v->numberDouble);
                case mongo::String: return std::strtoll(v->str.c_str(), nullptr, 10);
                default: return 0;
            }
        }

        BSONValuePtr decodeExtended(const BSONValuePtr &node);

        /** Returns a typed node if `obj` is an extended-JSON wrapper, else null */
        BSONValuePtr tryDecodeWrapper(const BSONValuePtr &obj) {
            if (obj->type != Object || obj->fields.empty()) return nullptr;
            const std::string &first = obj->fields[0].first;
            if (first.empty() || first[0] != '$') return nullptr;

            auto out = std::make_shared<BSONValue>();

            if (first == "$oid") {
                out->type = jstOID;
                out->oid = OID(obj->fields[0].second->str);
                return out;
            }
            if (first == "$numberInt") {
                out->type = NumberInt;
                out->numberInt = static_cast<int>(numberOf(obj->fields[0].second));
                return out;
            }
            if (first == "$numberLong") {
                out->type = NumberLong;
                out->numberLong = numberOf(obj->fields[0].second);
                return out;
            }
            if (first == "$numberDouble") {
                out->type = NumberDouble;
                const auto &v = obj->fields[0].second;
                if (v->type == mongo::String) {
                    if (v->str == "Infinity") out->numberDouble = HUGE_VAL;
                    else if (v->str == "-Infinity") out->numberDouble = -HUGE_VAL;
                    else if (v->str == "NaN") out->numberDouble = NAN;
                    else out->numberDouble = std::strtod(v->str.c_str(), nullptr);
                } else {
                    out->numberDouble = v->numberDouble;
                }
                return out;
            }
            if (first == "$numberDecimal") {
                out->type = NumberDecimal;
                out->str = obj->fields[0].second->str;
                return out;
            }
            if (first == "$date") {
                out->type = mongo::Date;
                const auto &v = obj->fields[0].second;
                if (v->type == Object) {
                    // canonical: {"$date": {"$numberLong": "..."}}
                    out->date = Date_t::fromMillisSinceEpoch(numberOf(field(v, "$numberLong")));
                } else if (v->type == mongo::String) {
                    // relaxed: ISO-8601 string
                    out->date = Date_t::fromMillisSinceEpoch(parseIsoDate(v->str));
                } else {
                    // legacy v1: milliseconds number
                    out->date = Date_t::fromMillisSinceEpoch(numberOf(v));
                }
                return out;
            }
            if (first == "$timestamp") {
                out->type = bsonTimestamp;
                const auto &v = obj->fields[0].second;
                out->timestamp = Timestamp(
                    static_cast<unsigned>(numberOf(field(v, "t"))),
                    static_cast<unsigned>(numberOf(field(v, "i"))));
                return out;
            }
            if (first == "$binary") {
                out->type = BinData;
                const auto &v = obj->fields[0].second;
                if (v->type == Object) {
                    // v2: {"$binary": {"base64": "...", "subType": "04"}}
                    auto b64 = field(v, "base64");
                    auto sub = field(v, "subType");
                    out->binary = base64::decode(b64 ? b64->str : "");
                    out->binSubType = static_cast<BinDataType>(
                        sub ? std::strtol(sub->str.c_str(), nullptr, 16) : 0);
                } else {
                    // v1: {"$binary": "...", "$type": "04"}
                    out->binary = base64::decode(v->str);
                    auto sub = field(obj, "$type");
                    out->binSubType = static_cast<BinDataType>(
                        sub ? std::strtol(sub->str.c_str(), nullptr, 16) : 0);
                }
                return out;
            }
            if (first == "$regularExpression") {
                out->type = RegEx;
                const auto &v = obj->fields[0].second;
                auto pattern = field(v, "pattern");
                auto options = field(v, "options");
                out->str = pattern ? pattern->str : "";
                out->str2 = options ? options->str : "";
                return out;
            }
            if (first == "$regex" && obj->fields[0].second->type == mongo::String) {
                // legacy v1 (only when $regex holds a string - avoid clobbering
                // query operators like {$regex: {...}})
                out->type = RegEx;
                out->str = obj->fields[0].second->str;
                auto options = field(obj, "$options");
                out->str2 = options ? options->str : "";
                return out;
            }
            if (first == "$undefined") {
                out->type = Undefined;
                return out;
            }
            if (first == "$minKey") {
                out->type = MinKey;
                return out;
            }
            if (first == "$maxKey") {
                out->type = MaxKey;
                return out;
            }
            if (first == "$symbol") {
                out->type = Symbol;
                out->str = obj->fields[0].second->str;
                return out;
            }
            if (first == "$code") {
                auto scope = field(obj, "$scope");
                if (scope) {
                    out->type = CodeWScope;
                    out->str = obj->fields[0].second->str;
                    out->scope = decodeExtended(scope);
                } else {
                    out->type = Code;
                    out->str = obj->fields[0].second->str;
                }
                return out;
            }
            return nullptr;
        }

        BSONValuePtr decodeExtended(const BSONValuePtr &node) {
            if (node->type == Object) {
                if (auto typed = tryDecodeWrapper(node))
                    return typed;
                auto obj = BSONValue::makeObject();
                for (const auto &f : node->fields)
                    obj->fields.emplace_back(f.first, decodeExtended(f.second));
                return obj;
            }
            if (node->type == mongo::Array) {
                auto arr = BSONValue::makeArray();
                for (const auto &f : node->fields)
                    arr->fields.emplace_back(f.first, decodeExtended(f.second));
                return arr;
            }
            return node;
        }

        long long parseIsoDate(const std::string &iso) {
            // Accepts YYYY-MM-DDTHH:MM:SS[.mmm](Z|±HH:MM)
            std::tm tmv{};
            int ms = 0;
            int consumed = 0;
            if (std::sscanf(iso.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%n", &tmv.tm_year, &tmv.tm_mon,
                            &tmv.tm_mday, &tmv.tm_hour, &tmv.tm_min, &tmv.tm_sec,
                            &consumed) != 6)
                return 0;
            tmv.tm_year -= 1900;
            tmv.tm_mon -= 1;
            size_t pos = static_cast<size_t>(consumed);
            if (pos < iso.size() && iso[pos] == '.') {
                size_t end = pos + 1;
                std::string frac;
                while (end < iso.size() && std::isdigit(static_cast<unsigned char>(iso[end])))
                    frac += iso[end++];
                while (frac.size() < 3) frac += '0';
                ms = std::atoi(frac.substr(0, 3).c_str());
                pos = end;
            }
            long long offsetSec = 0;
            if (pos < iso.size() && (iso[pos] == '+' || iso[pos] == '-')) {
                int oh = 0, om = 0;
                if (std::sscanf(iso.c_str() + pos + 1, "%2d:%2d", &oh, &om) >= 1)
                    offsetSec = (oh * 3600LL + om * 60LL) * (iso[pos] == '-' ? -1 : 1);
            }
#ifdef _WIN32
            long long secs = _mkgmtime(&tmv);
#else
            long long secs = timegm(&tmv);
#endif
            return (secs - offsetSec) * 1000 + ms;
        }

        // -------------------------------------------------------------
        // Serialization
        // -------------------------------------------------------------

        void serialize(const BSONValuePtr &v, std::ostream &s, bool canonical);

        void serializeDoc(const BSONValuePtr &v, std::ostream &s, bool canonical) {
            bool isArr = v->type == mongo::Array;
            s << (isArr ? "[" : "{");
            bool firstField = true;
            for (const auto &f : v->fields) {
                if (!firstField) s << ", ";
                firstField = false;
                if (!isArr) s << '"' << str::escape(f.first) << "\" : ";
                serialize(f.second, s, canonical);
            }
            s << (isArr ? "]" : "}");
        }

        void serialize(const BSONValuePtr &v, std::ostream &s, bool canonical) {
            switch (v->type) {
                case Object:
                case mongo::Array:
                    serializeDoc(v, s, canonical);
                    break;
                case mongo::String:
                    s << '"' << str::escape(v->str) << '"';
                    break;
                case Symbol:
                    if (canonical)
                        s << "{\"$symbol\": \"" << str::escape(v->str) << "\"}";
                    else
                        s << '"' << str::escape(v->str) << '"';
                    break;
                case NumberInt:
                    if (canonical)
                        s << "{\"$numberInt\": \"" << v->numberInt << "\"}";
                    else
                        s << v->numberInt;
                    break;
                case NumberLong:
                    if (canonical)
                        s << "{\"$numberLong\": \"" << v->numberLong << "\"}";
                    else
                        s << "NumberLong(" << v->numberLong << ")";
                    break;
                case NumberDouble:
                    if (canonical)
                        s << "{\"$numberDouble\": \"" << v->numberDouble << "\"}";
                    else
                        s << v->numberDouble;
                    break;
                case NumberDecimal:
                    s << "{\"$numberDecimal\": \"" << v->str << "\"}";
                    break;
                case mongo::Bool:
                    s << (v->boolean ? "true" : "false");
                    break;
                case jstNULL:
                    s << "null";
                    break;
                case Undefined:
                    s << (canonical ? "{\"$undefined\": true}" : "undefined");
                    break;
                case jstOID:
                    if (canonical)
                        s << "{\"$oid\": \"" << v->oid.toString() << "\"}";
                    else
                        s << "ObjectId(\"" << v->oid.toString() << "\")";
                    break;
                case mongo::Date:
                    if (canonical)
                        s << "{\"$date\": {\"$numberLong\": \""
                          << v->date.toMillisSinceEpoch() << "\"}}";
                    else
                        s << "ISODate(\"" << v->date.toString() << "\")";
                    break;
                case bsonTimestamp:
                    if (canonical)
                        s << "{\"$timestamp\": {\"t\": " << v->timestamp.getSecs()
                          << ", \"i\": " << v->timestamp.getInc() << "}}";
                    else
                        s << "Timestamp(" << v->timestamp.getSecs() << ", "
                          << v->timestamp.getInc() << ")";
                    break;
                case RegEx:
                    if (canonical)
                        s << "{\"$regularExpression\": {\"pattern\": \"" << str::escape(v->str)
                          << "\", \"options\": \"" << v->str2 << "\"}}";
                    else
                        s << "/" << v->str << "/" << v->str2;
                    break;
                case BinData: {
                    char sub[8];
                    std::snprintf(sub, sizeof(sub), "%02x", static_cast<int>(v->binSubType));
                    s << "{\"$binary\": {\"base64\": \""
                      << base64::encode(v->binary.data(), static_cast<int>(v->binary.size()))
                      << "\", \"subType\": \"" << sub << "\"}}";
                    break;
                }
                case Code:
                    s << "{\"$code\": \"" << str::escape(v->str) << "\"}";
                    break;
                case CodeWScope:
                    s << "{\"$code\": \"" << str::escape(v->str) << "\", \"$scope\": ";
                    if (v->scope) serializeDoc(v->scope, s, canonical);
                    else s << "{}";
                    s << "}";
                    break;
                case MinKey:
                    s << "{\"$minKey\": 1}";
                    break;
                case MaxKey:
                    s << "{\"$maxKey\": 1}";
                    break;
                case DBRef:
                    s << "DBRef(\"" << v->str << "\", \"" << v->str2 << "\")";
                    break;
                default:
                    s << "null";
            }
        }
    }

    BSONObj fromjson(const std::string &json) {
        JsonParser parser(json);
        BSONValuePtr plain = parser.parse();
        if (plain->type != Object && plain->type != mongo::Array)
            throw shelljson::ParseMsgAssertionException(
                "JSON text must be an object or array", 0);
        return BSONObj(decodeExtended(plain));
    }

    namespace shelljson {

        BSONObj fromjson(const std::string &json) {
            JsonParser parser(json, true /*relaxed shell notation*/);
            BSONValuePtr plain = parser.parse();
            if (plain->type != Object && plain->type != mongo::Array)
                throw ParseMsgAssertionException("JSON text must be an object or array", 0);
            return BSONObj(decodeExtended(plain));
        }

        BSONObj fromjson(const char *json, int *len) {
            const std::string text(json);
            JsonParser parser(text, true /*relaxed shell notation*/);
            BSONValuePtr plain = parser.parseOne();
            if (len)
                *len = static_cast<int>(parser.position());
            if (plain->type != Object && plain->type != mongo::Array)
                throw ParseMsgAssertionException("JSON text must be an object or array", 0);
            return BSONObj(decodeExtended(plain));
        }
    }

    std::string tojson(const BSONObj &obj, JsonStringFormat format, bool /*pretty*/) {
        std::ostringstream s;
        serializeDoc(obj.root(), s, format == Strict);
        return s.str();
    }

    std::string toCanonicalExtJson(const BSONObj &obj) {
        std::ostringstream s;
        serializeDoc(obj.root(), s, true);
        return s.str();
    }

    std::string BSONObj::toString() const {
        std::ostringstream s;
        serializeDoc(_root, s, false);
        return s.str();
    }

    std::string BSONObj::jsonString(JsonStringFormat format, int /*pretty*/) const {
        std::ostringstream s;
        serializeDoc(_root, s, format == Strict);
        return s.str();
    }

    std::string BSONElement::toString(bool includeFieldName) const {
        std::ostringstream s;
        if (includeFieldName && !_name.empty()) s << _name << ": ";
        if (_value) serialize(_value, s, false);
        else s << "EOO";
        return s.str();
    }

    std::string BSONElement::jsonString(JsonStringFormat format, bool includeFieldNames,
                                        int /*pretty*/) const {
        std::ostringstream s;
        if (includeFieldNames && !_name.empty())
            s << '"' << str::escape(_name) << "\" : ";
        if (_value) serialize(_value, s, format == Strict);
        return s.str();
    }
}
