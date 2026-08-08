#pragma once

// Runs the bundled mongosh as a per-evaluation sidecar process.
// This is the execution engine that replaced the embedded MongoDB 4.2
// shell (docs/Mongo6Modernization.md): each exec() spawns
//   mongosh <uri> --quiet --norc --json=canonical --eval <script>
// and parses stdout into print-output plus a structured EJSON result.

#include <QProcess>
#include <QString>
#include <QStringList>

#include <atomic>
#include <string>

#include "robomongo/core/engine/MongoshOutput.h"

namespace Robomongo
{
    class ConnectionSettings;

    class MongoshExecutor
    {
    public:
        struct EvalResult
        {
            MongoshEvalOutput output;

            /** stderr content (connection warnings, fatal launch errors) */
            std::string errorOutput;

            bool timedOut = false;

            /** mongosh could not be started at all */
            bool failedToStart = false;

            int exitCode = 0;
        };

        /**
         * Locates the mongosh binary: next to the application executable
         * (bundled by the installer), then ROBO_MONGOSH_PATH, then PATH.
         * Returns an empty string when not found.
         */
        static QString findMongoshBinary();

        /**
         * Builds the mongosh command-line arguments (connection string and
         * TLS/auth flags) for the given connection.
         *
         * serverAddr overrides host:port when non-empty - used when the
         * connection goes through Robo's SSH tunnel (localhost:<tunnel port>)
         * or targets a specific replica-set member.
         */
        static QStringList connectionArgs(ConnectionSettings *connection,
                                          const std::string &serverAddr,
                                          const std::string &dbName);

        /**
         * Evaluates one or more scripts in a single mongosh session
         * (multiple --eval arguments; the LAST one's value becomes the
         * --json result on stdout). Blocks up to timeoutSec (<=0: no
         * timeout); on timeout the process is killed and timedOut is set.
         */
        EvalResult eval(const QStringList &connectionArgs,
                        const std::vector<std::string> &scripts, int timeoutSec);

        /** Kills the currently running evaluation, if any (thread-safe). */
        void interrupt();

    private:
        std::atomic<QProcess *> _activeProcess{nullptr};
    };
}
