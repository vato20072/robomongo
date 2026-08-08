#include "robomongo/core/engine/MongoshOutput.h"

#include <vector>

namespace Robomongo
{
    namespace
    {
        /**
         * A thrown JS error serialized by --json mode has message/name/stack.
         * Server errors carry errmsg (+ optional codeName). Babel syntax
         * errors carry code=BABEL_PARSE_ERROR with loc.
         */
        bool detectError(const mongo::BSONObj &obj, std::string &messageOut)
        {
            if (obj.isArray())
                return false;

            const std::string code = obj.getStringField("code");
            if (code == "BABEL_PARSE_ERROR" || code == "ERR_ASSERTION") {
                std::string msg = obj.getStringField("message");
                messageOut = msg.empty() ? "Syntax error" : msg;
                return true;
            }

            if (obj.hasField("message") && obj.hasField("stack")) {
                const std::string name = obj.getStringField("name");
                const std::string msg = obj.getStringField("message");
                messageOut = name.empty() ? msg : name + ": " + msg;

                // Prefer server error details when present
                const std::string errmsg = obj.getStringField("errmsg");
                if (!errmsg.empty() && errmsg != msg)
                    messageOut += " (" + errmsg + ")";
                return true;
            }

            if (obj.hasField("errmsg") && obj.hasField("ok") &&
                obj.getField("ok").numberDouble() == 0) {
                messageOut = obj.getStringField("errmsg");
                return true;
            }

            return false;
        }
    }

    MongoshEvalOutput parseMongoshOutput(const std::string &out)
    {
        MongoshEvalOutput result;

        // Collect offsets of lines that begin (column 0) with '{' or '[' -
        // candidates for the start of the trailing EJSON block. mongosh
        // pretty-prints the --json result starting at column 0; print()
        // output lines precede it.
        std::vector<size_t> candidates;
        size_t lineStart = 0;
        while (lineStart <= out.size()) {
            if (lineStart < out.size() && (out[lineStart] == '{' || out[lineStart] == '['))
                candidates.push_back(lineStart);
            size_t nl = out.find('\n', lineStart);
            if (nl == std::string::npos)
                break;
            lineStart = nl + 1;
        }

        // Try candidates from the last one backwards: the result is the
        // longest trailing region that parses as one JSON value.
        for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
            const std::string tail = out.substr(*it);
            try {
                result.result = mongo::fromjson(tail);
                result.hasResult = true;
                result.text = out.substr(0, *it);
                result.isError = detectError(result.result, result.errorMessage);
                return result;
            } catch (const std::exception &) {
                // Not a complete JSON value from here - user print output
                // may itself start with '{'. Try an earlier candidate.
            }
        }

        // No parseable trailing value (e.g. result was `null`/`undefined`,
        // which --json prints as bare text, or the script only printed)
        result.text = out;
        return result;
    }
}
