#include "robomongo/core/mongodb/MongoClient.h"

#include <algorithm>
#include <stdexcept>

#include "robomongo/core/domain/MongoDocument.h"
#include "robomongo/core/engine/ScriptEngine.h"
#include "robomongo/core/utils/BsonUtils.h"

namespace
{
    using namespace Robomongo;

    /** Double-quoted JS string literal */
    std::string js(const std::string &s)
    {
        return "\"" + mongo::str::escape(s) + "\"";
    }

    /** Embeds a document into a script as EJSON.parse('<canonical>') */
    std::string ejson(const mongo::BSONObj &obj)
    {
        return "EJSON.parse(" + js(mongo::toCanonicalExtJson(obj)) + ")";
    }

    /** db.getSiblingDB("<db>").getCollection("<coll>") */
    std::string coll(const std::string &dbName, const std::string &collName)
    {
        return "db.getSiblingDB(" + js(dbName) + ").getCollection(" + js(collName) + ")";
    }

    std::string coll(const MongoNamespace &ns)
    {
        return coll(ns.databaseName(), ns.collectionName());
    }

    IndexInfo makeIndexInfoFromBsonObj(
        const MongoCollectionInfo &collection,
        const mongo::BSONObj &obj)
    {
        using namespace Robomongo::BsonUtils;
        IndexInfo info(collection);
        info._name = obj.getStringField("name");
        mongo::BSONObj keyObj = obj.getObjectField("key");
        if (keyObj.isValid())
            info._keys = jsonString(keyObj, mongo::TenGen, 1, DefaultEncoding, Utc);

        info._unique = obj.getBoolField("unique");
        info._backGround = obj.getBoolField("background");
        info._sparse = obj.getBoolField("sparse");
        info._ttl = obj.getIntField("expireAfterSeconds");
        info._defaultLanguage = obj.getStringField("default_language");
        info._languageOverride = obj.getStringField("language_override");
        mongo::BSONObj weightsObj = obj.getObjectField("weights");
        if (weightsObj.isValid())
            info._textWeights = jsonString(weightsObj, mongo::TenGen, 1, DefaultEncoding, Utc);

        return info;
    }

    /** Index creation script for addEditIndex */
    std::string createIndexScript(const IndexInfo &info)
    {
        mongo::BSONObjBuilder options;
        options.append("name", info._name);
        if (info._unique) options.appendBool("unique", true);
        if (info._backGround) options.appendBool("background", true);
        if (info._sparse) options.appendBool("sparse", true);
        if (!info._defaultLanguage.empty()) options.append("default_language", info._defaultLanguage);
        if (!info._languageOverride.empty()) options.append("language_override", info._languageOverride);
        if (!info._textWeights.empty()) {
            mongo::BSONObj weights = mongo::shelljson::fromjson(info._textWeights);
            if (!weights.isEmpty())
                options.append("weights", weights);
        }
        if (info._ttl > 0) options.append("expireAfterSeconds", info._ttl);

        mongo::BSONObj keys = mongo::shelljson::fromjson(
            info._keys.empty() ? "{}" : info._keys);

        return coll(info._collection.ns()) + ".createIndex(" + ejson(keys) + ", " +
               ejson(options.obj()) + ")";
    }
}

namespace Robomongo
{
    MongoClient::MongoClient(ScriptEngine *const engine) :
        _engine(engine) { }

    mongo::BSONObj MongoClient::eval(const std::string &script, const std::string &dbName) const
    {
        return _engine->evalCommand(script, dbName);
    }

    std::vector<std::string> MongoClient::getCollectionNamesWithDbname(const std::string &dbname) const
    {
        mongo::BSONObj result = eval(
            "db.getSiblingDB(" + js(dbname) + ").getCollectionNames()");

        std::vector<std::string> collNames;
        mongo::BSONObjIterator it(result);
        while (it.more()) {
            mongo::BSONElement e = it.next();
            if (e.type() == mongo::String)
                collNames.push_back(dbname + '.' + e.String());
        }
        std::sort(collNames.begin(), collNames.end());
        return collNames;
    }

    std::vector<std::string> MongoClient::getDatabaseNames() const
    {
        mongo::BSONObj result = eval(
            "(async () => (await db.adminCommand({listDatabases: 1, nameOnly: true}))"
            ".databases.map(d => d.name))()");

        std::vector<std::string> names;
        mongo::BSONObjIterator it(result);
        while (it.more()) {
            mongo::BSONElement e = it.next();
            if (e.type() == mongo::String)
                names.push_back(e.String());
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    float MongoClient::getVersion() const
    {
        const std::string version = dbVersionStr();
        if (version.empty())
            return 0.0f;
        // "6.0.14" -> 6.0f (major.minor only - matches historical behavior)
        const size_t firstDot = version.find('.');
        if (firstDot == std::string::npos)
            return std::stof(version);
        const size_t secondDot = version.find('.', firstDot + 1);
        return std::stof(version.substr(0, secondDot));
    }

    std::string MongoClient::dbVersionStr() const
    {
        return eval("(async () => ({v: await db.version()}))()").getStringField("v");
    }

    std::string MongoClient::getStorageEngineType() const
    {
        return eval(
            "(async () => { try { const s = await db.serverStatus(); "
            "return {e: s.storageEngine.name}; } catch (err) { return {e: ''}; } })()")
            .getStringField("e");
    }

    std::vector<MongoUser> MongoClient::getUsers(const std::string &dbName)
    {
        mongo::BSONObj result = eval(
            "db.getSiblingDB(" + js(dbName) + ").runCommand({usersInfo: 1})");

        if (result.getField("ok").numberDouble() == 0) {
            std::string errStr = result.getStringField("errmsg");
            throw std::runtime_error(errStr.empty() ? "Failed to load users." : errStr);
        }

        const float version = getVersion();
        std::vector<MongoUser> users;
        for (auto const& usr : result.getField("users").Array()) {
            if (usr.type() == mongo::Object)
                users.push_back(MongoUser(version, usr.embeddedObject()));
        }

        return users;
    }

    void MongoClient::createUser(const std::string &dbName, const MongoUser &user)
    {
        mongo::BSONObjBuilder cmd;
        cmd.append("createUser", user.name());
        cmd.append("pwd", user.password());

        mongo::BSONArrayBuilder roles;
        for (auto const& roleStr : user.roles()) {
            mongo::BSONObjBuilder role;
            role.append("role", roleStr);
            role.append("db", user.userSource());
            roles.append(role.obj());
        }
        cmd.appendArray("roles", roles.arr());

        mongo::BSONObj result = eval(
            "db.getSiblingDB(" + js(dbName) + ").runCommand(" + ejson(cmd.obj()) + ")");

        if (result.getField("ok").numberDouble() == 0) {
            std::string errStr = result.getStringField("errmsg");
            throw std::runtime_error(errStr.empty() ? "Failed to create user." : errStr);
        }
    }

    void MongoClient::dropUser(const std::string &dbName, const std::string &user)
    {
        mongo::BSONObj result = eval(
            "db.getSiblingDB(" + js(dbName) + ").runCommand({dropUser: " + js(user) + "})");

        if (result.getField("ok").numberDouble() == 0) {
            std::string errStr = result.getStringField("errmsg");
            throw std::runtime_error(errStr.empty() ? "Failed to drop user." : errStr);
        }
    }

    std::vector<MongoFunction> MongoClient::getFunctions(const std::string &dbName) const
    {
        mongo::BSONObj result = eval(
            "(async () => { const c = await " + coll(dbName, "system.js") + ".find(); "
            "return await c.toArray(); })()");

        std::vector<MongoFunction> functions;
        mongo::BSONObjIterator it(result);
        while (it.more()) {
            mongo::BSONElement e = it.next();
            if (e.type() != mongo::Object)
                continue;
            try {
                functions.push_back(MongoFunction(e.Obj()));
            } catch (const std::exception &) {
                // skip entries we cannot represent
            }
        }
        return functions;
    }

    std::vector<IndexInfo> MongoClient::getIndexes(const MongoCollectionInfo &collection) const
    {
        mongo::BSONObj result = eval(coll(collection.ns()) + ".getIndexes()");

        std::vector<IndexInfo> infos;
        mongo::BSONObjIterator it(result);
        while (it.more()) {
            mongo::BSONElement e = it.next();
            if (e.type() == mongo::Object)
                infos.push_back(makeIndexInfoFromBsonObj(collection, e.Obj()));
        }
        return infos;
    }

    void MongoClient::addEditIndex(const IndexInfo &oldInfo, const IndexInfo &newInfo) const
    {
        bool const editIndex = !oldInfo._name.empty();

        // MongoDB docs: to modify an existing index, drop and recreate it
        if (editIndex)
            dropIndexFromCollection(newInfo._collection, oldInfo._name);

        try {
            eval(createIndexScript(newInfo));
        }
        catch (std::exception const& /*ex*/) {  // Logged in upper scope
            if (editIndex) {
                // Recreation failed - try to at least restore the dropped index
                eval(createIndexScript(oldInfo));
            }
            throw;
        }
    }

    void MongoClient::renameIndexFromCollection(const MongoCollectionInfo &collection,
                                                const std::string &oldIndexName,
                                                const std::string &newIndexName) const
    {
        // No rename command exists: re-create the index under the new name
        eval(
            "(async () => {"
            "  const c = " + coll(collection.ns()) + ";"
            "  const idx = (await c.getIndexes()).find(i => i.name === " + js(oldIndexName) + ");"
            "  if (!idx) throw new Error('Index not found: ' + " + js(oldIndexName) + ");"
            "  const {v, key, name, ns, ...options} = idx;"
            "  await c.dropIndex(" + js(oldIndexName) + ");"
            "  await c.createIndex(key, {...options, name: " + js(newIndexName) + "});"
            "  return {ok: 1};"
            "})()");
    }

    void MongoClient::dropIndexFromCollection(const MongoCollectionInfo &collection,
                                              const std::string &indexName) const
    {
        eval(coll(collection.ns()) + ".dropIndex(" + js(indexName) + ")");
    }

    void MongoClient::createFunction(const std::string &dbName, const MongoFunction &fun,
                                     const std::string &existingFunctionName /* = "" */)
    {
        const std::string name = fun.name();

        // Renaming: remove the function stored under the old name first
        if (!existingFunctionName.empty() && existingFunctionName != name)
            dropFunction(dbName, existingFunctionName);

        eval(coll(dbName, "system.js") + ".replaceOne("
             "{_id: " + js(name) + "}, "
             "{_id: " + js(name) + ", value: new Code(" + js(fun.code()) + ")}, "
             "{upsert: true})");
    }

    void MongoClient::dropFunction(const std::string &dbName, const std::string &name)
    {
        eval(coll(dbName, "system.js") + ".deleteOne({_id: " + js(name) + "})");
    }

    void MongoClient::createDatabase(const std::string &dbName)
    {
        /*
         * MongoDB has no explicit "create database": create (and drop) a
         * temporary collection to make the database appear. The worker
         * additionally tracks created names client-side because empty
         * databases are not listed by the server.
         */
        eval(
            "(async () => {"
            "  const d = db.getSiblingDB(" + js(dbName) + ");"
            "  if ((await d.getCollectionNames()).includes('temp'))"
            "    throw new Error(" + js(dbName + ".temp already exists.") + ");"
            "  await d.getCollection('temp').insertOne({_id: 'temp'});"
            "  await d.getCollection('temp').drop();"
            "  return {ok: 1};"
            "})()");
    }

    void MongoClient::dropDatabase(const std::string &dbName)
    {
        mongo::BSONObj result = eval("db.getSiblingDB(" + js(dbName) + ").dropDatabase()");
        if (result.hasField("ok") && result.getField("ok").numberDouble() == 0) {
            std::string errStr = result.getStringField("errmsg");
            throw std::runtime_error(errStr.empty() ? "Failed to drop database." : errStr);
        }
    }

    void MongoClient::createCollection(const std::string &ns, long long size, bool capped,
                                       int max, const mongo::BSONObj &extraOptions,
                                       mongo::BSONObj *info /* = nullptr */)
    {
        MongoNamespace mongons(ns);

        mongo::BSONObjBuilder options;
        if (capped) {
            options.appendBool("capped", true);
            options.append("size", size);
            if (max > 0)
                options.append("max", static_cast<long long>(max));
        }
        options.appendElements(extraOptions);

        mongo::BSONObj result = eval(
            "db.getSiblingDB(" + js(mongons.databaseName()) + ").createCollection(" +
            js(mongons.collectionName()) + ", " + ejson(options.obj()) + ")");

        if (info)
            *info = result;

        if (result.hasField("ok") && result.getField("ok").numberDouble() == 0) {
            std::string errStr = result.getStringField("errmsg");
            throw std::runtime_error(errStr.empty() ? "Failed to create collection." : errStr);
        }
    }

    void MongoClient::renameCollection(const MongoNamespace &ns, const std::string &newCollectionName)
    {
        eval(coll(ns) + ".renameCollection(" + js(newCollectionName) + ")");
    }

    void MongoClient::duplicateCollection(const MongoNamespace &ns, const std::string &newCollectionName)
    {
        // Server-side copy via aggregation $out (the old implementation
        // looped documents through the embedded driver client-side)
        eval(
            "(async () => { const c = await " + coll(ns) + ".aggregate([{$match: {}}, {$out: " +
            js(newCollectionName) + "}]); return await c.toArray(); })()");
    }

    void MongoClient::dropCollection(const MongoNamespace &ns)
    {
        mongo::BSONObj result = eval(
            "(async () => ({dropped: await " + coll(ns) + ".drop()}))()");
        if (!result.getField("dropped").trueValue())
            throw std::runtime_error("Unable to drop collection " + ns.toString());
    }

    void MongoClient::insertDocument(const mongo::BSONObj &obj, const MongoNamespace &ns)
    {
        eval(coll(ns) + ".insertOne(" + ejson(obj) + ")");
    }

    void MongoClient::saveDocument(const mongo::BSONObj &obj, const MongoNamespace &ns)
    {
        mongo::BSONElement id = obj.getField("_id");
        if (id.eoo()) {
            insertDocument(obj, ns);
            return;
        }

        mongo::BSONObjBuilder query;
        query.append(id);

        eval(coll(ns) + ".replaceOne(" + ejson(query.obj()) + ", " + ejson(obj) +
             ", {upsert: true})");
    }

    void MongoClient::removeDocuments(const MongoNamespace &ns, mongo::Query query,
                                      bool justOne /* = true */)
    {
        const std::string method = justOne ? ".deleteOne(" : ".deleteMany(";
        eval(coll(ns) + method + ejson(query.obj()) + ")");
    }

    std::vector<MongoDocumentPtr> MongoClient::query(const MongoQueryInfo &info)
    {
        MongoNamespace ns(info._info._ns);

        std::vector<MongoDocumentPtr> docs;

        if (info._limit == -1)  // no documents requested
            return docs;

        // The legacy driver accepted "special" query envelopes
        // ({query: ..., orderby: ...}); unwrap them for find()/sort()
        mongo::BSONObj filter = info._query;
        mongo::BSONObj orderBy;
        if (info._special) {
            mongo::BSONObj inner = filter.getObjectField("query");
            if (inner.isEmpty())
                inner = filter.getObjectField("$query");
            mongo::BSONObj order = filter.getObjectField("orderby");
            if (order.isEmpty())
                order = filter.getObjectField("$orderby");
            if (!inner.isEmpty() || !order.isEmpty()) {
                filter = inner;
                orderBy = order;
            }
        }

        // Options are passed as find()'s third argument: cursor methods
        // (.limit/.skip/.sort) are async-rewriter proxies that cannot be
        // chained inside the session's eval() context
        mongo::BSONObjBuilder options;
        if (!orderBy.isEmpty())
            options.append("sort", orderBy);
        if (info._skip > 0)
            options.append("skip", info._skip);
        if (info._limit > 0)
            options.append("limit", info._limit);

        std::string script =
            "(async () => { const c = await " + coll(ns) + ".find(" + ejson(filter) + ", " +
            ejson(info._fields) + ", " + ejson(options.obj()) + "); "
            "return await c.toArray(); })()";

        mongo::BSONObj result = eval(script, ns.databaseName());

        mongo::BSONObjIterator it(result);
        while (it.more()) {
            mongo::BSONElement e = it.next();
            if (e.type() == mongo::Object)
                docs.push_back(MongoDocument::fromBsonObj(e.Obj()));
        }
        return docs;
    }

    MongoCollectionInfo MongoClient::runCollStatsCommand(const std::string &ns)
    {
        // Stats intentionally not loaded here (kept from the original
        // implementation, which disabled them to speed up collection loads)
        MongoCollectionInfo info(ns);
        return info;
    }

    std::vector<MongoCollectionInfo> MongoClient::runCollStatsCommand(const std::vector<std::string> &namespaces)
    {
        std::vector<MongoCollectionInfo> infos;
        for (auto const& ns : namespaces) {
            MongoCollectionInfo info = runCollStatsCommand(ns);
            if (info.ns().isValid())
                infos.push_back(info);
        }
        return infos;
    }

    void MongoClient::done()
    {
    }
}
