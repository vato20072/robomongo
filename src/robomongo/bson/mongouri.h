#pragma once

// Minimal MongoDB connection-string parsing for the URI import/export
// feature of the connection dialogs. Mirrors the subset of the legacy
// mongo::MongoURI / mongo::StatusWith API that Robo 3T uses.

#include <string>
#include <vector>

#include "robomongo/bson/bson.h"

namespace mongo {

    class Status {
    public:
        Status() = default;
        explicit Status(const std::string &reason) : _ok(false), _reason(reason) {}
        static Status OK() { return Status(); }
        bool isOK() const { return _ok; }
        const std::string &reason() const { return _reason; }
        std::string toString() const { return _ok ? "OK" : _reason; }
    private:
        bool _ok = true;
        std::string _reason;
    };

    template <typename T>
    class StatusWith {
    public:
        StatusWith(const T &value) : _value(value) {}
        StatusWith(const Status &status) : _status(status) {}
        bool isOK() const { return _status.isOK(); }
        const Status &getStatus() const { return _status; }
        const T &getValue() const { return _value; }
    private:
        T _value{};
        Status _status;
    };

    class ConnectionString {
    public:
        enum class ConnectionType { MASTER, SET, INVALID };

        ConnectionString() = default;
        explicit ConnectionString(const HostAndPort &server) : _servers{server} {}

        std::string toString() const {
            std::string out;
            for (size_t i = 0; i < _servers.size(); ++i) {
                if (i) out += ",";
                out += _servers[i].toString();
            }
            return out;
        }
    private:
        std::vector<HostAndPort> _servers;
    };

    /** Result of getOption: value-or-default accessor */
    class UriOptionValue {
    public:
        UriOptionValue() = default;
        UriOptionValue(const std::string &value) : _present(true), _value(value) {}
        std::string get_value_or(const std::string &fallback) const {
            return _present ? _value : fallback;
        }
    private:
        bool _present = false;
        std::string _value;
    };

    class MongoURI {
    public:
        /** Parses mongodb:// (and mongodb+srv://) connection strings */
        static StatusWith<MongoURI> parse(const std::string &uri);

        ConnectionString::ConnectionType type() const {
            return _isReplicaSet ? ConnectionString::ConnectionType::SET
                                 : ConnectionString::ConnectionType::MASTER;
        }

        const std::vector<HostAndPort> &getServers() const { return _servers; }
        const std::string &getSetName() const { return _setName; }
        const std::string &getUser() const { return _user; }
        const std::string &getPassword() const { return _password; }
        const std::string &getDatabase() const { return _database; }
        std::string getAuthenticationDatabase() const;
        UriOptionValue getOption(const std::string &name) const;
        BSONObj getOptions() const;
        std::string toString() const { return _original; }

    private:
        std::string _original;
        std::vector<HostAndPort> _servers;
        std::string _setName;
        std::string _user;
        std::string _password;
        std::string _database;
        std::vector<std::pair<std::string, std::string>> _options;
        bool _isReplicaSet = false;
    };
}
