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

    /**
     * A persistent mongosh REPL session: one process per connection, fed
     * scripts over stdin, results returned as sentinel-framed EJSON
     * envelopes. Eliminates the per-evaluation process spawn + TCP/TLS/auth
     * handshake (warm evaluations are milliseconds instead of seconds) and
     * restores shell-state persistence between executions.
     *
     * Not thread-safe: must be used from the thread that runs it first
     * (the MongoWorker thread owns the QProcess).
     */
    class MongoshSession
    {
    public:
        ~MongoshSession();

        struct RunOutcome
        {
            /** Session could not start or the process died mid-run */
            bool sessionError = false;
            std::string sessionErrorMessage;

            bool timedOut = false;

            /** Script evaluated without throwing */
            bool ok = false;
            std::string errorName;
            std::string errorMessage;

            bool hasResult = false;          // result is an object/array
            mongo::BSONObj result;
            bool scalarResult = false;       // non-document result
            std::string scalarText;

            /** print()/helper output preceding the envelope */
            std::string textOutput;

            /** {server, db, queryInfo, aggrInfo} reported by the shell */
            mongo::BSONObj meta;
            bool hasMeta = false;
        };

        /**
         * Runs a script in the session (starting/restarting the process as
         * needed). dbName switches the session's current database first;
         * isHelper sends the script as a raw REPL line (use/show/...).
         */
        RunOutcome run(const QStringList &connArgs, const std::string &script,
                       const std::string &dbName, bool isHelper, int timeoutSec);

        /** Kills the session process; the next run() starts a fresh one. */
        void interrupt();
        void shutdown();

    private:
        bool ensureStarted(const QStringList &connArgs, std::string *error);

        std::unique_ptr<QProcess> _process;
        QStringList _connArgs;
        long long _sequence = 0;
    };
}
