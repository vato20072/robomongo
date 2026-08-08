#include "robomongo/core/engine/ScriptEngine.h"

#include <QElapsedTimer>

#include <cctype>
#include <cstring>
#include <sstream>

#include "robomongo/bson/mongouri.h"
#include "robomongo/core/domain/MongoDocument.h"
#include "robomongo/core/settings/ConnectionSettings.h"
#include "robomongo/core/settings/CredentialSettings.h"
#include "robomongo/core/utils/Logger.h"

namespace
{
    /** True if the script is a mongosh REPL helper (not plain JavaScript) */
    bool isShellHelper(const std::string &script)
    {
        size_t i = 0;
        while (i < script.size() && std::isspace(static_cast<unsigned char>(script[i])))
            ++i;
        const std::string rest = script.substr(i);
        auto startsWith = [&](const char *kw) {
            return rest.rfind(kw, 0) == 0;
        };
        return startsWith("use ") || startsWith("show ") || startsWith("set ") ||
               rest == "it" || rest == "exit" || rest == "quit" || rest == "cls" ||
               rest == "help" || startsWith("help ");
    }

    /**
     * Produces a credential-free display address "mongodb://host:port/db"
     * for the shell tab header. mongosh reports its connection as a full
     * URI including username/password and internal options; those must not
     * be shown. Falls back to the raw string if it cannot be parsed.
     */
    std::string displayServer(const std::string &raw, const std::string &dbName)
    {
        auto statusWith = mongo::MongoURI::parse(raw);
        if (!statusWith.isOK())
            return raw;

        const mongo::MongoURI &uri = statusWith.getValue();
        std::string out = "mongodb://";
        const auto &servers = uri.getServers();
        for (size_t i = 0; i < servers.size(); ++i) {
            if (i) out += ",";
            out += servers[i].toString();
        }
        const std::string db = !dbName.empty() ? dbName : uri.getDatabase();
        if (!db.empty())
            out += "/" + db;
        return out;
    }

}

namespace Robomongo
{
    ScriptEngine::ScriptEngine(ConnectionSettings *connection, int timeoutSec) :
        _connection(connection),
        _timeoutSec(timeoutSec),
        _mutex(QMutex::Recursive) { }

    ScriptEngine::~ScriptEngine()
    {
    }

    void ScriptEngine::init(bool /*isLoadMongoRcJs*/, const std::string& serverAddr, const std::string& dbName)
    {
        QMutexLocker lock(&_mutex);

        _serverAddr = serverAddr;

        std::string connectDatabase = dbName.empty() ? "test" : dbName;
        if (_connection->hasEnabledPrimaryCredential() && dbName.empty())
            connectDatabase = _connection->primaryCredential()->databaseName();
        _currentDbName = connectDatabase;

        if (MongoshExecutor::findMongoshBinary().isEmpty()) {
            _failedScope = true;
            LOG_MSG("mongosh binary not found - shell execution unavailable. "
                    "Bundle mongosh next to the executable or set ROBO_MONGOSH_PATH.",
                    mongo::logger::LogSeverity::Error());
            return;
        }

        _failedScope = false;
        _initialized = true;
    }

    QStringList ScriptEngine::sessionArgs() const
    {
        // Fixed initial db: per-run db switching keeps the session reusable
        return MongoshExecutor::connectionArgs(_connection, _serverAddr, std::string());
    }

    ScriptEngine::ExecMeta ScriptEngine::extractMeta(const mongo::BSONObj &metaObj,
                                                     AggrInfo requestAggrInfo)
    {
        ExecMeta meta;

        try {
            meta.server = metaObj.getStringField("server");
            meta.serverValid = !meta.server.empty();
            meta.database = metaObj.getStringField("db");
            meta.databaseValid = !meta.database.empty();

            mongo::BSONElement qi = metaObj.getField("queryInfo");
            if (qi.type() == mongo::Object) {
                mongo::BSONObj q = qi.Obj();
                meta.queryInfo = MongoQueryInfo(
                    CollectionInfo(meta.server, q.getStringField("db"),
                                   q.getStringField("collection")),
                    q.getObjectField("query"), q.getObjectField("fields"),
                    q.getField("limit").numberInt(), q.getField("skip").numberInt(),
                    q.getField("batchSize").numberInt(), 0 /*options*/, false /*special*/);
                meta.hasQueryInfo = true;
            }

            mongo::BSONElement ai = metaObj.getField("aggrInfo");
            if (ai.type() == mongo::Object) {
                mongo::BSONObj a = ai.Obj();
                mongo::BSONObj pipeline = a.getObjectField("pipeline");
                // A paging re-run keeps the original (unpaged) pipeline
                mongo::BSONObj const origPipeline =
                    requestAggrInfo.isValid ? requestAggrInfo.pipeline : pipeline;
                int const skip = requestAggrInfo.isValid ? requestAggrInfo.skip : 0;
                int const batchSize = requestAggrInfo.isValid ? requestAggrInfo.batchSize : 50;
                int const resultIndex = requestAggrInfo.isValid ? requestAggrInfo.resultIndex : -1;
                meta.aggrInfo = AggrInfo(a.getStringField("collection"), skip, batchSize,
                                         origPipeline, a.getObjectField("options"), resultIndex);
            }
        } catch (const std::exception &e) {
            LOG_MSG(std::string("Failed to parse mongosh metadata: ") + e.what(),
                    mongo::logger::LogSeverity::Warning(), false);
        }

        return meta;
    }

    MongoShellExecResult ScriptEngine::exec(const std::string &originalScript,
                                            const std::string &dbName,
                                            AggrInfo aggrInfo /* = AggrInfo() */)
    {
        QMutexLocker lock(&_mutex);

        if (!_initialized) {
            _failedScope = true;
            return MongoShellExecResult(true, "Connection error. Shell engine is not initialized.");
        }

        if (!dbName.empty())
            _currentDbName = dbName;

        QElapsedTimer timer;
        timer.start();

        MongoshSession::RunOutcome outcome = _session.run(
            sessionArgs(), originalScript, _currentDbName,
            isShellHelper(originalScript) ? MongoshSession::Mode::Helper
                                          : MongoshSession::Mode::UserScript,
            _timeoutSec);

        const qint64 elapsed = timer.elapsed();

        if (outcome.sessionError)
            return MongoShellExecResult(true, outcome.sessionErrorMessage);

        if (outcome.timedOut)
            return MongoShellExecResult(true, "Script execution timed out.", true);

        ExecMeta meta = outcome.hasMeta ? extractMeta(outcome.meta, aggrInfo) : ExecMeta();

        // Track db switches (`use otherDb`) done inside the script
        if (meta.databaseValid)
            _currentDbName = meta.database;

        if (!outcome.ok) {
            std::string message = outcome.errorName.empty()
                ? outcome.errorMessage
                : outcome.errorName + ": " + outcome.errorMessage;
            if (!outcome.textOutput.empty())
                message = outcome.textOutput + "\n" + message;
            return MongoShellExecResult(true, message);
        }

        std::string textOutput = outcome.textOutput;
        if (outcome.scalarResult) {
            if (!textOutput.empty())
                textOutput += "\n";
            textOutput += outcome.scalarText;
        }

        // Build documents from the structured result
        std::vector<MongoDocumentPtr> documents;
        std::string type;
        if (outcome.hasResult) {
            if (outcome.result.isArray()) {
                type = "cursor";
                mongo::BSONObjIterator it(outcome.result);
                while (it.more()) {
                    mongo::BSONElement e = it.next();
                    if (e.type() == mongo::Object || e.type() == mongo::Array)
                        documents.push_back(MongoDocument::fromBsonObj(e.Obj()));
                    else if (!e.eoo())
                        textOutput += e.toString(false) + "\n";
                }
            } else {
                type = meta.hasQueryInfo ? "cursor" : "object";
                documents.push_back(MongoDocument::fromBsonObj(outcome.result));
            }
        }

        std::vector<MongoShellResult> results;
        if (!textOutput.empty() || !documents.empty()) {
            results.emplace_back(type, textOutput, documents,
                                 meta.hasQueryInfo ? meta.queryInfo : MongoQueryInfo(),
                                 originalScript, elapsed,
                                 meta.aggrInfo.isValid ? meta.aggrInfo : aggrInfo);
        }

        const std::string rawServer = meta.serverValid
            ? meta.server
            : (_serverAddr.empty() ? _connection->getFullAddress() : _serverAddr);
        const std::string server = displayServer(rawServer, _currentDbName);

        return MongoShellExecResult(results, server, meta.serverValid || !rawServer.empty(),
                                    _currentDbName, true, false);
    }

    mongo::BSONObj ScriptEngine::evalCommand(const std::string &script, const std::string &dbName)
    {
        QMutexLocker lock(&_mutex);

        if (!_initialized)
            throw std::runtime_error("Shell engine is not initialized.");

        MongoshSession::RunOutcome outcome = _session.run(
            sessionArgs(), script, dbName.empty() ? _currentDbName : dbName,
            MongoshSession::Mode::Internal, _timeoutSec);

        if (outcome.sessionError)
            throw std::runtime_error(outcome.sessionErrorMessage);
        if (outcome.timedOut)
            throw std::runtime_error("Operation timed out.");
        if (!outcome.ok)
            throw std::runtime_error(outcome.errorMessage.empty() ? "mongosh evaluation failed"
                                                                  : outcome.errorMessage);

        return outcome.result;
    }

    void ScriptEngine::interrupt()
    {
        _session.interrupt();
    }

    void ScriptEngine::use(const std::string &dbName)
    {
        QMutexLocker lock(&_mutex);
        if (!dbName.empty())
            _currentDbName = dbName;
    }

    void ScriptEngine::setBatchSize(int batchSize)
    {
        QMutexLocker lock(&_mutex);
        _batchSize = batchSize;
    }

    void ScriptEngine::ping()
    {
        // Sessions are per-execution: nothing to keep alive.
    }

    QStringList ScriptEngine::complete(const std::string &prefix, const AutocompletionMode mode)
    {
        QMutexLocker lock(&_mutex);

        // Static shell vocabulary + cached collection names. (The embedded
        // 4.2 shell used its own autocompletion machinery; mongosh has an
        // autocomplete API that could be integrated later.)
        static const QStringList kShellVocabulary = {
            "db", "rs", "sh", "use", "show",
            "db.getCollection(", "db.getCollectionNames()", "db.getName()",
            "db.stats()", "db.serverStatus()", "db.version()", "db.dropDatabase()",
            "db.createCollection(", "db.currentOp()", "db.killOp(",
            "find(", "findOne(", "insertOne(", "insertMany(", "updateOne(",
            "updateMany(", "deleteOne(", "deleteMany(", "aggregate(",
            "countDocuments(", "estimatedDocumentCount()", "distinct(",
            "createIndex(", "getIndexes()", "dropIndex(", "drop()", "renameCollection(",
            "sort(", "limit(", "skip(", "projection(", "toArray()", "forEach(",
            "ObjectId(", "ISODate(", "NumberLong(", "NumberInt(", "NumberDecimal(",
            "UUID(", "Timestamp(",
        };

        QStringList candidates = kShellVocabulary;

        if (mode == AutocompleteAll) {
            if (!_collectionCacheValid && _initialized) {
                MongoshSession::RunOutcome res = _session.run(
                    sessionArgs(), "db.getCollectionNames()", _currentDbName,
                    MongoshSession::Mode::Internal, 5);
                if (res.ok && res.hasResult && res.result.isArray()) {
                    _cachedCollectionNames.clear();
                    mongo::BSONObjIterator it(res.result);
                    while (it.more()) {
                        mongo::BSONElement e = it.next();
                        if (e.type() == mongo::String)
                            _cachedCollectionNames
                                << QString::fromStdString("db." + e.String());
                    }
                    _collectionCacheValid = true;
                }
            }
            candidates += _cachedCollectionNames;
        }

        const QString qprefix = QString::fromStdString(prefix);
        QStringList results;
        for (const QString &candidate : candidates)
            if (candidate.startsWith(qprefix, Qt::CaseInsensitive) && candidate != qprefix)
                results << candidate;
        results.removeDuplicates();
        results.sort(Qt::CaseInsensitive);
        return results;
    }

    void ScriptEngine::invalidateDbCollectionsCache()
    {
        QMutexLocker lock(&_mutex);
        _collectionCacheValid = false;
        _cachedCollectionNames.clear();
    }
}
