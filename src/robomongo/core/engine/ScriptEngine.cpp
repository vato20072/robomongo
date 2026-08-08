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
    const char *kMetaMarker = "__ROBO_META__";

    /**
     * Prelude evaluated before the user script in the same mongosh session.
     * - Patches Collection.find/aggregate to capture query metadata
     *   (collection, filter, projection, limit/skip) for Robo's paging UI.
     * - On process exit writes a __ROBO_META__ EJSON line to stderr with the
     *   captured metadata and the final db/server (stdout stays reserved for
     *   the --json result of the user script).
     */
    const char *kPrelude = R"ROBO(
(() => {
  globalThis.__robo = { queryInfo: null, aggrInfo: null };
  try {
    const sampleColl = db.getCollection('__robo_probe__');
    const collProto = Object.getPrototypeOf(sampleColl);

    const origFind = collProto.find;
    collProto.find = function (filter, projection, options) {
      const cursor = origFind.call(this, filter, projection, options);
      try {
        globalThis.__robo.queryInfo = {
          db: (this._database && this._database.getName) ? this._database.getName() : db.getName(),
          collection: this.getName ? this.getName() : String(this._name),
          query: filter || {},
          fields: projection || {},
          limit: 0, skip: 0, batchSize: 0
        };
        const curProto = Object.getPrototypeOf(cursor);
        if (!curProto.__roboPatched) {
          curProto.__roboPatched = true;
          for (const m of ['limit', 'skip', 'batchSize']) {
            const orig = curProto[m];
            if (typeof orig === 'function') {
              curProto[m] = function (v) {
                if (globalThis.__robo.queryInfo) globalThis.__robo.queryInfo[m] = v;
                return orig.apply(this, arguments);
              };
            }
          }
        }
      } catch (e) { /* metadata is best-effort */ }
      return cursor;
    };

    const origAggregate = collProto.aggregate;
    collProto.aggregate = function (pipeline, options) {
      try {
        globalThis.__robo.aggrInfo = {
          collection: this.getName ? this.getName() : String(this._name),
          pipeline: pipeline || [],
          options: options || {}
        };
      } catch (e) { /* best-effort */ }
      return origAggregate.apply(this, arguments);
    };
  } catch (e) { /* no db (--nodb) or unexpected shell API - continue */ }

  process.on('exit', () => {
    try {
      const safe = (fn) => { try { return fn(); } catch (e) { return null; } };
      const meta = {
        server: safe(() => db.getMongo()._uri) || safe(() => db.getMongo().toString()),
        db: safe(() => db.getName()),
        queryInfo: globalThis.__robo.queryInfo,
        aggrInfo: globalThis.__robo.aggrInfo
      };
      process.stderr.write('\n__ROBO_META__' + EJSON.stringify(meta, { relaxed: false }) + '\n');
    } catch (e) { /* stdout result already delivered */ }
  });
})();
)ROBO";
}

namespace
{
    // Maximum documents materialized from a cursor in the shell result pane
    // (mongosh --json cannot serialize a live cursor, and toArray() on a huge
    // collection would exhaust memory). Robo's table view uses its own paged
    // query path; this only bounds ad-hoc find()s typed in a shell tab.
    constexpr int kShellCursorLimit = 1000;

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

    /**
     * Wraps a user shell script so mongosh --json can serialize its result:
     * evaluates it, awaits promises (async driver ops), and materializes
     * cursors into a bounded array of documents. Shell helpers (use/show/...)
     * are passed through unchanged since they are not valid JavaScript.
     */
    std::string wrapUserScript(const std::string &script)
    {
        if (isShellHelper(script))
            return script;

        // The script is embedded as a JS string and run with eval() so the
        // completion value of its last expression is captured. eval() does
        // not get mongosh's implicit-await transform, so we await explicitly.
        std::ostringstream ss;
        ss << "(async () => {"
              "  let __robo_v = eval(\"" << mongo::str::escape(script) << "\");"
              "  if (__robo_v && typeof __robo_v.then === 'function') __robo_v = await __robo_v;"
              "  if (__robo_v && typeof __robo_v.toArray === 'function'"
              "      && typeof __robo_v.hasNext === 'function') {"
              "    const __robo_docs = [];"
              "    while (__robo_docs.length < " << kShellCursorLimit
           << "        && await __robo_v.hasNext()) __robo_docs.push(await __robo_v.next());"
              "    __robo_v = __robo_docs;"
              "  }"
              "  return __robo_v;"
              "})()";
        return ss.str();
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

    ScriptEngine::ExecMeta ScriptEngine::extractMeta(const std::string &stderrText,
                                                     AggrInfo requestAggrInfo)
    {
        ExecMeta meta;

        const size_t markerPos = stderrText.find(kMetaMarker);
        if (markerPos == std::string::npos) {
            meta.cleanedStderr = stderrText;
            return meta;
        }

        const size_t jsonStart = markerPos + std::strlen(kMetaMarker);
        size_t jsonEnd = stderrText.find('\n', jsonStart);
        if (jsonEnd == std::string::npos)
            jsonEnd = stderrText.size();

        meta.cleanedStderr = stderrText.substr(0, markerPos) + stderrText.substr(jsonEnd);

        try {
            mongo::BSONObj metaObj =
                mongo::fromjson(stderrText.substr(jsonStart, jsonEnd - jsonStart));

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

        const QStringList connArgs =
            MongoshExecutor::connectionArgs(_connection, _serverAddr, _currentDbName);

        QElapsedTimer timer;
        timer.start();

        MongoshExecutor::EvalResult evalResult =
            _executor.eval(connArgs, {kPrelude, wrapUserScript(originalScript)}, _timeoutSec);

        const qint64 elapsed = timer.elapsed();

        if (evalResult.failedToStart)
            return MongoShellExecResult(true, evalResult.errorOutput);

        if (evalResult.timedOut)
            return MongoShellExecResult(true, "Script execution timed out.", true);

        ExecMeta meta = extractMeta(evalResult.errorOutput, aggrInfo);

        // Track db switches (`use otherDb`) done inside the script
        if (meta.databaseValid)
            _currentDbName = meta.database;

        const MongoshEvalOutput &out = evalResult.output;

        if (out.isError) {
            std::string message = out.errorMessage;
            if (!out.text.empty())
                message = out.text + "\n" + message;
            return MongoShellExecResult(true, message);
        }

        // Surface remaining stderr (connection warnings etc.) into output
        std::string textOutput = out.text;
        if (!meta.cleanedStderr.empty() && evalResult.exitCode != 0)
            textOutput += meta.cleanedStderr;

        // Build documents from the structured result
        std::vector<MongoDocumentPtr> documents;
        std::string type;
        if (out.hasResult) {
            if (out.result.isArray()) {
                type = "cursor";
                mongo::BSONObjIterator it(out.result);
                while (it.more()) {
                    mongo::BSONElement e = it.next();
                    if (e.type() == mongo::Object || e.type() == mongo::Array)
                        documents.push_back(MongoDocument::fromBsonObj(e.Obj()));
                    else if (!e.eoo())
                        textOutput += e.toString(false) + "\n";
                }
            } else {
                type = meta.hasQueryInfo ? "cursor" : "object";
                documents.push_back(MongoDocument::fromBsonObj(out.result));
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

        const QStringList connArgs = MongoshExecutor::connectionArgs(
            _connection, _serverAddr, dbName.empty() ? _currentDbName : dbName);

        MongoshExecutor::EvalResult res = _executor.eval(connArgs, {script}, _timeoutSec);

        if (res.failedToStart)
            throw std::runtime_error(res.errorOutput);
        if (res.timedOut)
            throw std::runtime_error("Operation timed out.");
        if (res.output.isError)
            throw std::runtime_error(res.output.errorMessage);
        if (!res.output.hasResult && res.exitCode != 0) {
            std::string message = res.errorOutput.empty() ? res.output.text : res.errorOutput;
            // Strip the __ROBO_META__ line if the prelude was not used
            throw std::runtime_error(message.empty() ? "mongosh evaluation failed" : message);
        }

        return res.output.result;
    }

    void ScriptEngine::interrupt()
    {
        _executor.interrupt();
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
                const QStringList connArgs =
                    MongoshExecutor::connectionArgs(_connection, _serverAddr, _currentDbName);
                MongoshExecutor::EvalResult res = _executor.eval(
                    connArgs, {"db.getCollectionNames()"}, 5);
                if (res.output.hasResult && res.output.result.isArray()) {
                    _cachedCollectionNames.clear();
                    mongo::BSONObjIterator it(res.output.result);
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
