#pragma once

// Parsing of `mongosh --quiet --json=canonical --eval` output into
// Robo 3T's result model. Pure C++ (no Qt) so it is unit-testable
// standalone.

#include <string>

#include "robomongo/bson/bson.h"

namespace Robomongo
{
    struct MongoshEvalOutput
    {
        /** print()/console output preceding the result value */
        std::string text;

        /** True when a trailing EJSON value was found and parsed */
        bool hasResult = false;

        /** The eval result. Top-level arrays are marked with isArray() */
        mongo::BSONObj result;

        /** True when the result value is a thrown error / server error shape */
        bool isError = false;

        /** Human-readable error (name: message, or errmsg) when isError */
        std::string errorMessage;
    };

    /**
     * Splits mongosh stdout into leading print-output and the trailing
     * EJSON result document emitted by --json mode, and recognizes
     * error shapes (thrown JS errors, Babel parse errors, server errors).
     */
    MongoshEvalOutput parseMongoshOutput(const std::string &stdoutText);
}
