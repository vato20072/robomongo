#pragma once

#include <QObject>
#include <QMutex>
#include <QStringList>

#include "robomongo/core/domain/MongoShellResult.h"
#include "robomongo/core/engine/MongoshExecutor.h"
#include "robomongo/core/Enums.h"

namespace Robomongo
{
    class ConnectionSettings;

    /**
     * Executes user scripts through the bundled mongosh (sidecar process
     * per evaluation) instead of the historical embedded MongoDB 4.2
     * shell. Public interface preserved from the original engine so
     * MongoWorker/MongoShell stay unchanged.
     */
    class ScriptEngine : public QObject
    {
        Q_OBJECT

    public:
        ScriptEngine(ConnectionSettings *connection, int timeoutSec);
        ~ScriptEngine();

        void init(bool isLoadMongoJs, const std::string& serverAddr = "", const std::string& dbName = "");
        MongoShellExecResult exec(const std::string &script, const std::string &dbName = std::string(),
                                  AggrInfo aggrInfo = AggrInfo());
        void interrupt();

        void use(const std::string &dbName);
        void setBatchSize(int batchSize);
        void ping();
        QStringList complete(const std::string &prefix, const AutocompletionMode mode);

        void invalidateDbCollectionsCache();

        /**
         * Runs a script and returns its structured result. Used by
         * MongoClient for data-layer operations (list databases, CRUD,
         * indexes...). Throws std::runtime_error on evaluation errors,
         * timeouts and process failures.
         */
        mongo::BSONObj evalCommand(const std::string &script, const std::string &dbName = std::string());

        bool failedScope() const { return _failedScope; }

        void changeTimeout(int newTimeout) { _timeoutSec = newTimeout; }

    private:
        /** Parses the __ROBO_META__ line out of mongosh stderr */
        struct ExecMeta
        {
            std::string server;
            std::string database;
            bool serverValid = false;
            bool databaseValid = false;
            MongoQueryInfo queryInfo;
            bool hasQueryInfo = false;
            AggrInfo aggrInfo;
            std::string cleanedStderr;
        };
        ExecMeta extractMeta(const std::string &stderrText, AggrInfo requestAggrInfo);

        ConnectionSettings *_connection;
        MongoshExecutor _executor;

        int _timeoutSec;
        std::string _serverAddr;     // SSH tunnel / replica-member override
        std::string _currentDbName;
        int _batchSize = 50;
        bool _failedScope = false;
        bool _initialized = false;
        QMutex _mutex;

        QStringList _cachedCollectionNames;
        bool _collectionCacheValid = false;
    };
}
