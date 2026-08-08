#pragma once

// robobson: a compact, mongo-API-compatible BSON document model backed by an
// order-preserving DOM instead of binary BSON bytes.
//
// Why this exists: Robo 3T historically linked the entire MongoDB 4.2 server
// codebase to get mongo::BSONObj and friends. After the mongosh pivot
// (docs/Mongo6Modernization.md) documents enter and leave the application as
// Extended JSON text produced/consumed by the bundled mongosh, so the only
// thing the GUI needs is a faithful in-memory document tree with the mongo::
// API surface the existing widgets already use. Binary BSON never occurs.
//
// The API intentionally lives in namespace mongo so the ~35 existing consumer
// files keep compiling. Only the subset of the original API that Robo 3T uses
// is provided.

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace mongo {

    enum BSONType {
        MinKey = -1,
        EOO = 0,
        NumberDouble = 1,
        String = 2,
        Object = 3,
        Array = 4,
        BinData = 5,
        Undefined = 6,
        jstOID = 7,
        Bool = 8,
        Date = 9,
        jstNULL = 10,
        RegEx = 11,
        DBRef = 12,
        Code = 13,
        Symbol = 14,
        CodeWScope = 15,
        NumberInt = 16,
        bsonTimestamp = 17,
        NumberLong = 18,
        NumberDecimal = 19,
        MaxKey = 127
    };

    enum BinDataType {
        BinDataGeneral = 0,
        Function = 1,
        ByteArrayDeprecated = 2,
        bdtUUID = 3,
        newUUID = 4,
        MD5Type = 5,
        bdtCustom = 128
    };

    enum JsonStringFormat { Strict, TenGen, JS };

    class OID {
    public:
        OID() = default;
        explicit OID(const std::string &hex) : _hex(hex) {}
        static OID gen();
        const std::string &toString() const { return _hex; }
        bool isSet() const { return !_hex.empty(); }
        bool operator==(const OID &o) const { return _hex == o._hex; }
        bool operator!=(const OID &o) const { return _hex != o._hex; }
    private:
        std::string _hex;
    };
    std::ostream &operator<<(std::ostream &s, const OID &o);

    class Date_t {
    public:
        Date_t() = default;
        static Date_t fromMillisSinceEpoch(long long ms) { Date_t d; d._millis = ms; return d; }
        long long toMillisSinceEpoch() const { return _millis; }
        std::string toString() const;
        bool operator==(const Date_t &o) const { return _millis == o._millis; }
    private:
        long long _millis = 0;
    };

    class Timestamp {
    public:
        Timestamp() = default;
        Timestamp(unsigned secs, unsigned inc) : _secs(secs), _inc(inc) {}
        unsigned getSecs() const { return _secs; }
        unsigned getInc() const { return _inc; }
    private:
        unsigned _secs = 0;
        unsigned _inc = 0;
    };

    /** Stored and displayed as its decimal string - never used arithmetically in the GUI */
    class Decimal128 {
    public:
        Decimal128() : _str("0") {}
        explicit Decimal128(const std::string &s) : _str(s) {}
        const std::string &toString() const { return _str; }
    private:
        std::string _str;
    };

    namespace detail {

        /** One node of the document tree. Field order is preserved. */
        struct BSONValue {
            BSONType type = jstNULL;

            // Scalar payloads (member-per-kind; only the one matching `type` is meaningful)
            double numberDouble = 0.0;
            long long numberLong = 0;
            int numberInt = 0;
            bool boolean = false;
            std::string str;            // String, Symbol, Code, Decimal128 text, DBRef ns
            std::string str2;           // RegEx options, DBRef OID hex
            OID oid;
            Date_t date;
            Timestamp timestamp;
            std::string binary;         // BinData payload (raw bytes)
            BinDataType binSubType = BinDataGeneral;
            std::shared_ptr<BSONValue> scope;  // CodeWScope scope document

            // Object / Array children
            std::vector<std::pair<std::string, std::shared_ptr<BSONValue>>> fields;

            static std::shared_ptr<BSONValue> makeObject() {
                auto v = std::make_shared<BSONValue>();
                v->type = Object;
                return v;
            }
            static std::shared_ptr<BSONValue> makeArray() {
                auto v = std::make_shared<BSONValue>();
                v->type = Array;
                return v;
            }
        };

        using BSONValuePtr = std::shared_ptr<BSONValue>;
    }

    class BSONObj;

    class BSONElement {
    public:
        BSONElement() = default;  // eoo element
        BSONElement(const std::string &name, detail::BSONValuePtr value)
            : _name(name), _value(std::move(value)) {}

        bool eoo() const { return !_value; }
        BSONType type() const { return _value ? _value->type : EOO; }
        const char *fieldName() const { return _name.c_str(); }
        std::string fieldNameStr() const { return _name; }

        bool isNull() const { return type() == jstNULL; }
        bool isUndefined() const { return type() == Undefined; }

        // --- string-ish ---
        std::string String() const;
        std::string str() const { return String(); }
        const char *valuestr() const;
        int valuestrsize() const;

        // --- numbers ---
        double numberDouble() const;
        double Double() const { return numberDouble(); }
        double number() const { return numberDouble(); }
        int numberInt() const;
        int Int() const { return numberInt(); }
        int _numberInt() const { return numberInt(); }
        long long numberLong() const;
        long long Long() const { return numberLong(); }
        long long _numberLong() const { return numberLong(); }
        long long safeNumberLong() const { return numberLong(); }
        Decimal128 numberDecimal() const;
        Decimal128 _numberDecimal() const { return numberDecimal(); }

        // --- other scalars ---
        bool Bool() const;
        bool boolean() const { return Bool(); }
        bool trueValue() const;
        mongo::OID OID() const;
        mongo::OID __oid() const { return OID(); }
        Date_t date() const;
        Date_t Date() const { return date(); }
        Timestamp timestamp() const;
        unsigned timestampInc() const { return timestamp().getInc(); }
        Date_t timestampTime() const;
        const char *regex() const;
        const char *regexFlags() const;
        std::string _asCode() const;

        // --- binary ---
        BinDataType binDataType() const;
        const char *binData(int &len) const;

        // --- documents ---
        BSONObj embeddedObject() const;
        BSONObj Obj() const;
        BSONObj codeWScopeObject() const;
        std::vector<BSONElement> Array() const;

        // --- DBRef ---
        std::string dbrefNS() const;
        std::string dbrefOID() const;

        std::string toString(bool includeFieldName = true) const;
        std::string jsonString(JsonStringFormat format = TenGen, bool includeFieldNames = true,
                               int pretty = 0) const;

        detail::BSONValuePtr valuePtr() const { return _value; }

    private:
        std::string _name;
        detail::BSONValuePtr _value;
    };

    class BSONObj {
    public:
        /** Empty document */
        BSONObj();
        explicit BSONObj(detail::BSONValuePtr root);

        bool isEmpty() const;
        bool isValid() const { return true; }
        int nFields() const;
        bool hasField(const std::string &name) const { return !getField(name).eoo(); }
        bool hasElement(const std::string &name) const { return hasField(name); }

        BSONElement getField(const std::string &name) const;
        BSONElement operator[](const std::string &name) const { return getField(name); }
        BSONElement firstElement() const;
        std::string getStringField(const std::string &name) const;
        int getIntField(const std::string &name) const;
        bool getBoolField(const std::string &name) const;
        BSONObj getObjectField(const std::string &name) const;

        /** Robo fork extension: BSONObj can be marked as a top-level array */
        bool isArray() const;
        void markAsArray();

        BSONObj getOwned() const { return *this; }
        BSONObj copy() const { return *this; }

        std::string toString() const;
        std::string jsonString(JsonStringFormat format = Strict, int pretty = 0) const;

        detail::BSONValuePtr root() const { return _root; }

        bool operator==(const BSONObj &other) const { return toString() == other.toString(); }

    private:
        detail::BSONValuePtr _root;
    };

    class BSONObjIterator {
    public:
        explicit BSONObjIterator(const BSONObj &obj) : _root(obj.root()) {}
        bool more() const;
        BSONElement next();
    private:
        detail::BSONValuePtr _root;
        size_t _index = 0;
    };

    /** STL-style iteration support (obj.begin()/obj.end()) */
    class BSONObjStlIterator {
    public:
        BSONObjStlIterator(detail::BSONValuePtr root, size_t index) : _root(root), _index(index) {}
        BSONElement operator*() const;
        BSONObjStlIterator &operator++() { ++_index; return *this; }
        bool operator!=(const BSONObjStlIterator &o) const { return _index != o._index; }
        bool operator==(const BSONObjStlIterator &o) const { return _index == o._index; }
    private:
        detail::BSONValuePtr _root;
        size_t _index;
    };
    BSONObjStlIterator begin(const BSONObj &obj);
    BSONObjStlIterator end(const BSONObj &obj);

    class BSONArray;

    class BSONObjBuilder {
    public:
        BSONObjBuilder();

        BSONObjBuilder &append(const std::string &name, const std::string &value);
        BSONObjBuilder &append(const std::string &name, const char *value);
        BSONObjBuilder &append(const std::string &name, int value);
        BSONObjBuilder &append(const std::string &name, long long value);
        BSONObjBuilder &append(const std::string &name, double value);
        BSONObjBuilder &append(const std::string &name, bool value);
        BSONObjBuilder &append(const std::string &name, const BSONObj &value);
        BSONObjBuilder &append(const std::string &name, const mongo::OID &value);
        BSONObjBuilder &append(const std::string &name, const Date_t &value);
        BSONObjBuilder &append(const std::string &name, const Timestamp &value);
        BSONObjBuilder &append(const std::string &name, const Decimal128 &value);
        BSONObjBuilder &append(const BSONElement &elem);
        BSONObjBuilder &appendArray(const std::string &name, const BSONObj &array);
        BSONObjBuilder &appendElements(const BSONObj &obj);
        BSONObjBuilder &appendBool(const std::string &name, bool value) { return append(name, value); }
        BSONObjBuilder &appendNumber(const std::string &name, long long value) { return append(name, value); }
        BSONObjBuilder &appendNumber(const std::string &name, int value) { return append(name, value); }
        BSONObjBuilder &appendNumber(const std::string &name, double value) { return append(name, value); }
        BSONObjBuilder &appendDate(const std::string &name, const Date_t &value) { return append(name, value); }
        BSONObjBuilder &appendNull(const std::string &name);
        BSONObjBuilder &appendUndefined(const std::string &name);
        BSONObjBuilder &appendMinKey(const std::string &name);
        BSONObjBuilder &appendMaxKey(const std::string &name);
        BSONObjBuilder &appendRegex(const std::string &name, const std::string &pattern,
                                    const std::string &options = "");
        BSONObjBuilder &appendBinData(const std::string &name, int len, BinDataType type,
                                      const char *data);
        BSONObjBuilder &appendCode(const std::string &name, const std::string &code);
        BSONObjBuilder &appendCodeWScope(const std::string &name, const std::string &code,
                                         const BSONObj &scope);
        BSONObjBuilder &appendTimestamp(const std::string &name, unsigned secs, unsigned inc);

        BSONObj obj();

    private:
        detail::BSONValuePtr _root;
        friend class BSONArrayBuilder;
    };

    class BSONArray : public BSONObj {
    public:
        BSONArray() { markAsArray(); }
        explicit BSONArray(const BSONObj &obj) : BSONObj(obj.root()) { markAsArray(); }
    };

    class BSONArrayBuilder {
    public:
        BSONArrayBuilder();

        BSONArrayBuilder &append(const std::string &value);
        BSONArrayBuilder &append(const char *value);
        BSONArrayBuilder &append(int value);
        BSONArrayBuilder &append(long long value);
        BSONArrayBuilder &append(double value);
        BSONArrayBuilder &append(bool value);
        BSONArrayBuilder &append(const BSONObj &value);
        BSONArrayBuilder &append(const BSONElement &elem);

        BSONArray arr();

    private:
        detail::BSONValuePtr _root;
    };

    // ---------------------------------------------------------------------
    // JSON / Extended JSON
    // ---------------------------------------------------------------------

    /**
     * Parses JSON text into a document tree. Understands MongoDB Extended
     * JSON v2 (canonical and relaxed - what mongosh --json emits) as well as
     * the legacy v1 forms ({$oid}, {$date: <ms>}, {$binary,$type}, ...).
     * Throws std::runtime_error with a descriptive message on parse failure.
     */
    BSONObj fromjson(const std::string &json);

    /** Compact one-line JSON (Strict-ish) - legacy mongo::tojson replacement */
    std::string tojson(const BSONObj &obj, JsonStringFormat format = Strict, bool pretty = false);

    /**
     * Serializes a document tree to canonical Extended JSON v2 - the format
     * sent to mongosh (safe to embed in an eval script).
     */
    std::string toCanonicalExtJson(const BSONObj &obj);

    // ---------------------------------------------------------------------
    // Small utilities mirrored from mongo/util - used by Robo code
    // ---------------------------------------------------------------------

    class StringBuilder {
    public:
        template <typename T>
        StringBuilder &operator<<(const T &v) { _ss << v; return *this; }
        std::string str() const { return _ss.str(); }
        void reset() { _ss.str(std::string()); _ss.clear(); }
    private:
        std::ostringstream _ss;
    };

    namespace str {
        /** Escapes a string for embedding in double-quoted JSON */
        std::string escape(const std::string &in, bool escape_slash = false);
    }

    namespace base64 {
        std::string encode(const char *data, int len);
        void encode(std::stringstream &ss, const char *data, int len);
        std::string decode(const std::string &encoded);
    }

    std::string toHexLower(const void *in, int len);

    /** Minimal StatusWith-alike so HexUtils' fromHex(p).getValue() keeps working */
    struct HexParseResult {
        char value;
        char getValue() const { return value; }
    };
    HexParseResult fromHex(const char *twoHexChars);

    namespace logger {
        class LogSeverity {
        public:
            static LogSeverity Severe()  { return LogSeverity(0); }
            static LogSeverity Error()   { return LogSeverity(1); }
            static LogSeverity Warning() { return LogSeverity(2); }
            static LogSeverity Info()    { return LogSeverity(3); }
            static LogSeverity Log()     { return LogSeverity(4); }
            static LogSeverity Debug()   { return LogSeverity(5); }

            int toInt() const { return _severity; }
            bool operator==(const LogSeverity &o) const { return _severity == o._severity; }
            bool operator!=(const LogSeverity &o) const { return _severity != o._severity; }
            std::string toString() const;

            LogSeverity() : _severity(3) {}  // default Info; required for Qt signal marshalling
        private:
            explicit LogSeverity(int severity) : _severity(severity) {}
            int _severity;
        };
    }

    class HostAndPort {
    public:
        HostAndPort() = default;
        HostAndPort(const std::string &host, int port) : _host(host), _port(port) {}
        /** Parses "host:port" (port defaults to 27017 when absent) */
        explicit HostAndPort(const std::string &hostPort) {
            const size_t colon = hostPort.rfind(':');
            if (colon == std::string::npos) {
                _host = hostPort;
            } else {
                _host = hostPort.substr(0, colon);
                try { _port = std::stoi(hostPort.substr(colon + 1)); } catch (...) {}
            }
        }
        const std::string &host() const { return _host; }
        int port() const { return _port; }
        bool empty() const { return _host.empty(); }
        std::string toString() const;
    private:
        std::string _host;
        int _port = 27017;
    };

    using StringData = std::string;

    /** Minimal stand-in for the legacy driver's Query wrapper */
    class Query {
    public:
        Query() = default;
        explicit Query(const BSONObj &obj) : _obj(obj) {}
        const BSONObj &obj() const { return _obj; }
    private:
        BSONObj _obj;
    };

    /** The 4.2 fork exposed a relaxed parser as mongo::Robomongo::fromjson */
    namespace Robomongo {
        inline BSONObj fromjson(const std::string &json) { return mongo::fromjson(json); }
    }
}
