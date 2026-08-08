#include "robomongo/bson/mongouri.h"

#include <algorithm>

namespace mongo {

    namespace {
        /** Percent-decodes a URI component */
        std::string uriDecode(const std::string &in)
        {
            std::string out;
            out.reserve(in.size());
            for (size_t i = 0; i < in.size(); ++i) {
                if (in[i] == '%' && i + 2 < in.size()) {
                    out += static_cast<char>(
                        std::strtol(in.substr(i + 1, 2).c_str(), nullptr, 16));
                    i += 2;
                } else {
                    out += in[i];
                }
            }
            return out;
        }
    }

    StatusWith<MongoURI> MongoURI::parse(const std::string &uriText)
    {
        MongoURI uri;
        uri._original = uriText;

        std::string rest = uriText;
        const std::string schemes[] = {"mongodb://", "mongodb+srv://"};
        bool schemeFound = false;
        for (const auto &scheme : schemes) {
            if (rest.compare(0, scheme.size(), scheme) == 0) {
                rest = rest.substr(scheme.size());
                schemeFound = true;
                break;
            }
        }
        if (!schemeFound)
            return Status("URI must begin with mongodb:// or mongodb+srv://");

        // credentials@
        const size_t at = rest.rfind('@', rest.find('/'));
        if (at != std::string::npos) {
            const std::string credentials = rest.substr(0, at);
            rest = rest.substr(at + 1);
            const size_t colon = credentials.find(':');
            if (colon == std::string::npos) {
                uri._user = uriDecode(credentials);
            } else {
                uri._user = uriDecode(credentials.substr(0, colon));
                uri._password = uriDecode(credentials.substr(colon + 1));
            }
        }

        // hosts[/database][?options]
        std::string hostsPart = rest;
        std::string pathPart;
        const size_t slash = rest.find('/');
        if (slash != std::string::npos) {
            hostsPart = rest.substr(0, slash);
            pathPart = rest.substr(slash + 1);
        } else {
            const size_t q = rest.find('?');
            if (q != std::string::npos) {
                hostsPart = rest.substr(0, q);
                pathPart = "?" + rest.substr(q + 1);
            }
        }

        if (hostsPart.empty())
            return Status("URI must specify at least one host");

        size_t start = 0;
        while (start <= hostsPart.size()) {
            size_t comma = hostsPart.find(',', start);
            const std::string host =
                hostsPart.substr(start, comma == std::string::npos ? std::string::npos
                                                                   : comma - start);
            if (!host.empty())
                uri._servers.push_back(HostAndPort(host));
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
        if (uri._servers.empty())
            return Status("URI must specify at least one host");

        // database?options
        std::string optionsPart;
        if (!pathPart.empty()) {
            const size_t q = pathPart.find('?');
            if (q == std::string::npos) {
                uri._database = uriDecode(pathPart);
            } else {
                uri._database = uriDecode(pathPart.substr(0, q));
                optionsPart = pathPart.substr(q + 1);
            }
        }

        start = 0;
        while (start < optionsPart.size()) {
            size_t amp = optionsPart.find('&', start);
            const std::string pair =
                optionsPart.substr(start, amp == std::string::npos ? std::string::npos
                                                                   : amp - start);
            const size_t eq = pair.find('=');
            if (eq != std::string::npos)
                uri._options.emplace_back(pair.substr(0, eq), uriDecode(pair.substr(eq + 1)));
            if (amp == std::string::npos)
                break;
            start = amp + 1;
        }

        const std::string setName = uri.getOption("replicaSet").get_value_or("");
        uri._setName = setName;
        uri._isReplicaSet = !setName.empty() || uri._servers.size() > 1;

        return uri;
    }

    std::string MongoURI::getAuthenticationDatabase() const
    {
        const std::string authSource = getOption("authSource").get_value_or("");
        if (!authSource.empty())
            return authSource;
        return _database.empty() ? "admin" : _database;
    }

    UriOptionValue MongoURI::getOption(const std::string &name) const
    {
        // Option names are case-insensitive per the connection string spec
        auto lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
            return s;
        };
        const std::string wanted = lower(name);
        for (const auto &option : _options)
            if (lower(option.first) == wanted)
                return UriOptionValue(option.second);
        return UriOptionValue();
    }

    BSONObj MongoURI::getOptions() const
    {
        BSONObjBuilder builder;
        for (const auto &option : _options)
            builder.append(option.first, option.second);
        return builder.obj();
    }
}
