#include "robomongo/core/mongodb/MongoWorker.h"

#include <algorithm>
#include <exception>
#include <set>

#include <QThread>

#include "robomongo/core/AppRegistry.h"
#include "robomongo/core/domain/App.h"
#include "robomongo/core/domain/MongoShellResult.h"
#include "robomongo/core/domain/MongoCollectionInfo.h"
#include "robomongo/core/events/MongoEvents.h"
#include "robomongo/core/engine/ScriptEngine.h"
#include "robomongo/core/EventBus.h"
#include "robomongo/core/mongodb/MongoClient.h"
#include "robomongo/core/settings/ConnectionSettings.h"
#include "robomongo/core/settings/ReplicaSetSettings.h"
#include "robomongo/core/settings/CredentialSettings.h"
#include "robomongo/core/settings/SettingsManager.h"
#include "robomongo/core/settings/SslSettings.h"
#include "robomongo/core/utils/BsonUtils.h"
#include "robomongo/core/utils/Logger.h"
#include "robomongo/core/utils/QtUtils.h"
#include "robomongo/utils/StringOperations.h"

namespace Robomongo
{
    std::string const APP_VERSION = PROJECT_VERSION;
    std::string const APP_NAME_VERSION { "robo3t-" + APP_VERSION };

    MongoWorker::MongoWorker(ConnectionSettings *connection, bool isLoadMongoRcJs, int batchSize,
                             double mongoTimeoutSec, int shellTimeoutSec, QObject *parent)
        : QObject(parent),
        _scriptEngine(nullptr),
        _isLoadMongoRcJs(isLoadMongoRcJs),
        _batchSize(batchSize),
        _timerId(-1),
        _dbAutocompleteCacheTimerId(-1),
        _mongoTimeoutSec(mongoTimeoutSec),
        _shellTimeoutSec(shellTimeoutSec),
        _isQuiting(0),
        _connSettings(connection)
    {
        // Whitespace removed from the start and the end of host string
        _connSettings->setServerHost(QString::fromStdString(_connSettings->serverHost()).trimmed().toStdString());
        _thread = new QThread();
        moveToThread(_thread);
        VERIFY(connect( _thread, SIGNAL(finished()), _thread, SLOT(deleteLater()) ));
        VERIFY(connect( _thread, SIGNAL(finished()), this, SLOT(deleteLater()) ));
        _thread->start();
    }

    void MongoWorker::timerEvent(QTimerEvent *event)
    {
        if (_timerId == event->timerId()) {
            keepAlive();
            return;
        }

        if (_dbAutocompleteCacheTimerId == event->timerId() && _scriptEngine) {
            _scriptEngine->invalidateDbCollectionsCache();
            return;
        }
    }

    void MongoWorker::keepAlive()
    {
        // mongosh sessions are created per evaluation: there is no live
        // connection to keep alive anymore.
    }

    void MongoWorker::init()
    {
        try {
            if (_scriptEngine)
                return;

            _scriptEngine.reset(new ScriptEngine(_connSettings, _shellTimeoutSec));
            _scriptEngine->init(_isLoadMongoRcJs);
            _scriptEngine->use(_connSettings->defaultDatabase());
            _scriptEngine->setBatchSize(_batchSize);
            constexpr int PING_INTERVAL_MSEC { 60 * 1000 };  // 60 seconds
            _timerId = startTimer(PING_INTERVAL_MSEC);
            _dbAutocompleteCacheTimerId = startTimer(30000);
        } catch (const std::exception &ex) {
            auto const msg { "Failed to initialize MongoWorker. Reason: "};
            sendLog(this, LogEvent::RBM_ERROR, msg + std::string(ex.what()));
            throw std::runtime_error(msg + std::string(ex.what()));
        }
    }

    void MongoWorker::interrupt() {
        try {
            if (_isQuiting || !_scriptEngine)
                return;

            _scriptEngine->interrupt();
        } catch(const std::exception &ex) {
            sendLog(this, LogEvent::RBM_ERROR, std::string(ex.what()));
        }
    }

    MongoWorker::~MongoWorker()
    {
        if (_timerId != -1)
            killTimer(_timerId);

        if (_dbAutocompleteCacheTimerId != -1)
            killTimer(_dbAutocompleteCacheTimerId);

        delete _connSettings;

        // QThread "_thread" and MongoWorker itself will be deleted later
        // (see MongoWorker() constructor)
    }

    void MongoWorker::stopAndDelete()
    {
        _isQuiting = 1;
        _thread->quit();
    }

    void MongoWorker::changeTimeout(int newTimeout)
    {
        _scriptEngine->changeTimeout(newTimeout);
    }

    /**
     * @brief Initiate connection to MongoDB
     */
    bool MongoWorker::handle(EstablishConnectionRequest *event)
    {
        QMutexLocker lock(&_firstConnectionMutex);

        std::unique_ptr<ReplicaSet> repSetInfo(new ReplicaSet);
        auto errorCode = EventError::ErrorCode::Unknown;

        try {
            init();

            // Probe connectivity with a ping through mongosh
            bool reachable = false;
            std::string connErrorStr;
            try {
                _scriptEngine->evalCommand("db.runCommand({ping: 1})", getAuthBase());
                reachable = true;
            } catch (const std::exception &ex) {
                connErrorStr = ex.what();
            }

            // --- Connection failed for single server & replica set (no member reachable)
            if (!reachable)
            {
                auto errorReason = std::string("Connection failure: Unknown error.");

                if (_connSettings->sslSettings()->sslEnabled())
                     errorReason =
                         "TLS tunnel failure: Network is unreachable or TLS connection rejected by server." +
                          (connErrorStr.empty() ? "" : " Reason: " + connErrorStr);
                else {  // Non-TLS connections
                    if (_connSettings->isReplicaSet()) {
                        errorReason = "No member of the set is reachable." +
                                      (connErrorStr.empty() ? "" : " Reason: " + connErrorStr);
                        std::vector<std::pair<std::string, bool>> membersAndHealths;
                        for (auto const& member : _connSettings->replicaSetSettings()->members())
                            membersAndHealths.push_back({ member, false });

                        repSetInfo.reset(new ReplicaSet("", mongo::HostAndPort(), membersAndHealths, errorReason));
                    }
                    else    // single server
                        errorReason = "Network is unreachable." + (connErrorStr.empty() ? "" : " Reason: " + connErrorStr);
                }

                reply(event->sender(), new EstablishConnectionResponse(this, EventError(errorReason, errorCode),
                      event->connectionType, event->uuid, *repSetInfo.release(),
                      EstablishConnectionResponse::MongoConnection));

                return false;
            }

            // --- Replica set: verify primary reachable and members consistent
            if (_connSettings->isReplicaSet()) {
                ReplicaSet const& setInfo = getReplicaSetInfo();

                // Remember the set name for subsequent connection strings
                if (!setInfo.setName.empty())
                    _connSettings->replicaSetSettings()->setCachedSetName(setInfo.setName);

                // Check if same set name used with different members which is not supported
                auto const& members = _connSettings->replicaSetSettings()->members();
                if (!setInfo.primary.empty() &&
                    std::find(members.cbegin(), members.cend(), setInfo.primary.toString()) == members.cend())
                {   // primary not found between user entered members
                    std::string const errorStr {
                        "Different members found under same replica set name \"" +
                         setInfo.setName + "\"."
                    };
                    repSetInfo.reset(new ReplicaSet(setInfo));
                    errorCode = EventError::ErrorCode::ServerHasDifferentMembers;
                    sendLog(this, LogEvent::RBM_ERROR, errorStr);
                    throw std::runtime_error(errorStr);
                }

                if (setInfo.primary.empty()) {  // No reachable primary
                    // Pass possible reachable secondary(ies) info
                    repSetInfo.reset(new ReplicaSet(setInfo));
                    sendLog(this, LogEvent::RBM_ERROR, setInfo.errorStr);
                    throw std::runtime_error(setInfo.errorStr);
                }
                else {  // Primary is reachable, save setInfo and continue
                    repSetInfo.reset(new ReplicaSet(setInfo));
                }
            }

            boost::scoped_ptr<MongoClient> client(getClient());
            std::vector<std::string> const dbNames = getDatabaseNamesSafe(event);

            // If we do not have databases, it means that we are unable to
            // execute "listdatabases" command and we have nothing to show.
            if (dbNames.size() == 0)
                throw std::runtime_error("Failed to execute \"listdatabases\" command.");

            auto connInfo = ConnectionInfo(_connSettings->getFullAddress(), dbNames, client->getVersion(),
                                           client->dbVersionStr(), client->getStorageEngineType(), event->uuid);

            // todo: two ctors for rep.set and single server.
            reply(event->sender(), new EstablishConnectionResponse(this, connInfo, event->connectionType,
                                                                   *repSetInfo.release()));
            return true;
        }
        catch(const std::exception &ex) {
            auto errorReason = _connSettings->sslSettings()->sslEnabled() ?
                               EstablishConnectionResponse::ErrorReason::MongoSslConnection :
                               EstablishConnectionResponse::ErrorReason::MongoAuth;

            reply(event->sender(), new EstablishConnectionResponse(this, EventError(ex.what(), errorCode),
                  event->connectionType, ConnectionInfo(event->uuid), *repSetInfo.release(), errorReason));
            sendLog(this, LogEvent::RBM_ERROR, ex.what());  // todo: duplicate logging?
        }

        return false;
    }

    void MongoWorker::handle(RefreshReplicaSetFolderRequest *event)
    {
        try {
            ReplicaSet const& replicaSetInfo = getReplicaSetInfo();
            // Primary is unreachable, but there might be reachable secondary(ies)
            if (replicaSetInfo.primary.empty()) {
                reply(
                    event->sender(),
                    new RefreshReplicaSetFolderResponse(
                        this, replicaSetInfo, event->expanded, EventError(replicaSetInfo.errorStr)
                    )
                );
                sendLog(this, LogEvent::RBM_ERROR, replicaSetInfo.errorStr);
                return;
            }
            else { // Primary is reachable
                reply(event->sender(),
                    new RefreshReplicaSetFolderResponse(this, replicaSetInfo, event->expanded));
            }
        }
        catch (const std::exception &ex) {
            reply(
                event->sender(),
                new RefreshReplicaSetFolderResponse(
                    this, ReplicaSet(), event->expanded, EventError(ex.what())
                )
            );
            sendLog(this, LogEvent::RBM_ERROR, ex.what());
        }
    }

    std::string MongoWorker::getAuthBase() const
    {
        if (_connSettings->hasEnabledPrimaryCredential())
            return _connSettings->primaryCredential()->databaseName();

        return std::string();
    }

    std::vector<std::string> MongoWorker::getDatabaseNamesSafe(EstablishConnectionRequest* event /*= nullptr*/)
    {
        std::set<std::string> dbNames;
        auto const primaryCredential { _connSettings->primaryCredential() };

        try {
            boost::scoped_ptr<MongoClient> client(getClient());
            std::vector<std::string> dbNamesFetched { client->getDatabaseNames() };
            dbNames = std::set<std::string> { dbNamesFetched.cbegin(), dbNamesFetched.cend() };
        }
        catch(const std::exception &ex) {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlogical-op-parentheses"
#endif
            bool const informUser {
                event != nullptr &&
                event->connectionType == ConnectionType::ConnectionPrimary &&
                _connSettings->credentialCount() > 0 &&
                !primaryCredential->useManuallyVisibleDbs() ||
                primaryCredential->manuallyVisibleDbs().empty()
            };
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
            std::string const hint {
                "\n\nHint: If this user has access to a specific database, "
                "please use \"Manually specify visible databases\" option in "
                "Connection Settings window -> Authentication tab."
            };
            sendLog(this, LogEvent::RBM_WARN, ex.what() + hint, informUser);
        }

        if (_connSettings->credentialCount() > 0 &&
            primaryCredential->useManuallyVisibleDbs() &&
            !primaryCredential->manuallyVisibleDbs().empty()
        )
        {
            QString const manuallyVisibleDbs {
                QString::fromStdString(primaryCredential->manuallyVisibleDbs())
            };

            for (auto const& db : manuallyVisibleDbs.split(',').toStdList())
                dbNames.insert(db.toStdString());
        }

        std::string const authBase = getAuthBase();
        if (!authBase.empty())
            dbNames.insert(authBase);

        return std::vector<std::string> { dbNames.cbegin(), dbNames.cend() };
    }

    /**
     * @brief Load list of all database names
     */
    void MongoWorker::handle(LoadDatabaseNamesRequest *event)
    {
        try {
            // If user not an admin - he doesn't have access to mongodb 'listDatabases' command
            // Non admin user has access only to the single database he specified while performing auth.
            std::vector<std::string> dbNames = getDatabaseNamesSafe();

            // Remove from list of created databases existing databases
            for (std::vector<std::string>::iterator it = dbNames.begin(); it != dbNames.end(); ++it) {
                std::unordered_set<std::string>::const_iterator exists = _createdDbs.find(*it);
                if (exists != _createdDbs.end()) {
                    _createdDbs.erase(*it);
                }
            }

            // Merge with list of created databases
            for (std::unordered_set<std::string>::iterator it = _createdDbs.begin(); it != _createdDbs.end(); ++it) {
                dbNames.push_back(*it);
            }

            if (dbNames.size()) {
                reply(event->sender(), new LoadDatabaseNamesResponse(this, dbNames));
            } else {
                auto const errorStr{ "Failed to execute \"listdatabases\" command." };
                reply(event->sender(), new LoadDatabaseNamesResponse(this, EventError(errorStr)));
            }
        } catch(const std::exception &ex) {
            reply(event->sender(), new LoadDatabaseNamesResponse(this, EventError(ex.what())));
            sendLog(this, LogEvent::RBM_ERROR, ex.what());
        }
    }

    /**
     * @brief Load list of all collection names
     */
    void MongoWorker::handle(LoadCollectionNamesRequest *event)
    {
        try {
            boost::scoped_ptr<MongoClient> client(getClient());
            auto const& namespaces = client->getCollectionNamesWithDbname(event->databaseName());
            std::vector<MongoCollectionInfo> const& collInfos = client->runCollStatsCommand(namespaces);
            client->done();
            reply(event->sender(), new LoadCollectionNamesResponse(this, event->databaseName(), collInfos));
        } catch(const std::exception &ex) {
            reply(event->sender(), new LoadCollectionNamesResponse(this, EventError(ex.what())));
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(LoadUsersRequest *event)
    {
        try {
            boost::scoped_ptr<MongoClient> client(getClient());
            const std::vector<MongoUser> &users = client->getUsers(event->databaseName());
            client->done();

            reply(event->sender(), new LoadUsersResponse(this, event->databaseName(), users));
        } catch(const std::exception &ex) {
            reply(event->sender(), new LoadUsersResponse(this, EventError(ex.what())));
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(LoadCollectionIndexesRequest *event)
    {
        try {
            boost::scoped_ptr<MongoClient> client(getClient());
            const std::vector<IndexInfo> &ind = client->getIndexes(event->collection());
            client->done();

            reply(event->sender(), new LoadCollectionIndexesResponse(this, ind));
        } catch(const std::exception &ex) {
            reply(event->sender(), new LoadCollectionIndexesResponse(this, EventError(ex.what())));
            sendLog(this, LogEvent::RBM_ERROR, ex.what());
        }
    }

    void MongoWorker::handle(AddEditIndexRequest *event)
    {
        const IndexInfo &newIndex = event->newInfo();
        const IndexInfo &oldIndex = event->oldInfo();
        try {
            boost::scoped_ptr<MongoClient> client(getClient());
            client->addEditIndex(oldIndex, newIndex);
            client->done();
            reply(event->sender(), new AddEditIndexResponse(this, oldIndex, newIndex));

            std::vector<IndexInfo> const &indexes = client->getIndexes(newIndex._collection);
            reply(event->sender(), new LoadCollectionIndexesResponse(this, indexes));
        } catch(const std::exception &ex) {
            reply(event->sender(),
                new AddEditIndexResponse(this, EventError(ex.what()), oldIndex, newIndex)
            );
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(DropCollectionIndexRequest *event)
    {
        try {
            boost::scoped_ptr<MongoClient> client(getClient());
            client->dropIndexFromCollection(event->collection(), event->index());
            client->done();
            reply(event->sender(),
                new DropCollectionIndexResponse(this, event->collection(), event->index()));
        } catch(const std::exception &ex) {
            reply(event->sender(),
                new DropCollectionIndexResponse(this, EventError(ex.what()), event->index()));
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(LoadFunctionsRequest *event)
    {
        try {
            boost::scoped_ptr<MongoClient> client(getClient());
            const std::vector<MongoFunction> &funcs = client->getFunctions(event->databaseName());
            client->done();

            reply(event->sender(), new LoadFunctionsResponse(this, event->databaseName(), funcs));
        } catch(const std::exception &ex) {
            reply(event->sender(), new LoadFunctionsResponse(this, EventError(ex.what())));
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(InsertDocumentRequest *event)
    {
        try {
            boost::scoped_ptr<MongoClient> client(getClient());

            if (event->overwrite())
                client->saveDocument(event->obj(), event->ns());
            else
                client->insertDocument(event->obj(), event->ns());

            client->done();
            reply(event->sender(), new InsertDocumentResponse(this));
        }
        catch(const std::exception &ex) {
            reply(event->sender(), new InsertDocumentResponse(this, EventError(ex.what())));
            sendLog(this, LogEvent::RBM_ERROR, ex.what());
        }
    }

    void MongoWorker::handle(RemoveDocumentRequest *event)
    {
        try {
            boost::scoped_ptr<MongoClient> client(getClient());

            client->removeDocuments(event->ns(), event->query(),
                                    event->removeCount() == RemoveDocumentCount::ONE);
            client->done();

            reply(event->sender(), new RemoveDocumentResponse(this, event->removeCount(), event->index()));
        }
        catch(const std::exception &ex) {
            reply(event->sender(), new RemoveDocumentResponse(this, EventError(ex.what()),
                event->removeCount(), event->index()));
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(ExecuteQueryRequest *event)
    {
        try {
            boost::scoped_ptr<MongoClient> client { getClient() };
            std::vector<MongoDocumentPtr> docs = client->query(event->queryInfo());
            client->done();
            reply(event->sender(),
                new ExecuteQueryResponse(this, event->resultIndex(), event->queryInfo(), docs)
            );
        } catch(const std::exception &ex) {
            reply(event->sender(), new ExecuteQueryResponse(this, EventError(ex.what())));
            sendLog(this, LogEvent::RBM_ERROR, std::string(ex.what()));
        }
    }

    /**
     * @brief Execute javascript
     */
    void MongoWorker::handle(ExecuteScriptRequest *event)
    {
        try {
            if (!_scriptEngine) {
                auto const error{
                    EventError("MongoDB Shell was not initialized or connection failure")
                };
                reply(event->sender(), new ExecuteScriptResponse(this, error));
                return;
            }

            // Try to handle case where new shell (which was opened when server unreachable)
            // was re-executed
            if (_scriptEngine->failedScope()) {
                try {
                    _scriptEngine->init(_isLoadMongoRcJs);
                }
                catch (std::exception const& ex) {
                    sendLog(this, LogEvent::RBM_ERROR,
                        captilizeFirstChar(ex.what()) + ", cannot init mongo scope");
                }
            }

            // todo: should we use dbName from event or _connSettings?
            MongoShellExecResult result {
                _scriptEngine->exec(
                    event->script, _connSettings->defaultDatabase(), event->aggrInfo
                )
            };

            if (result.error()) {
                auto const error { EventError(result.errorMessage()) };
                reply(event->sender(), new ExecuteScriptResponse(this, error));
                return;
            }

            reply(
                event->sender(),
                new ExecuteScriptResponse(this, result, event->script.empty(),
                    result.timeoutReached()) // todo: rename to shellTimeout...
            );
        }
        catch(const std::exception &ex) {
            auto const error { EventError(ex.what(), EventError::Unknown) };
            reply(event->sender(), new ExecuteScriptResponse(this, error));
            sendLog(this, LogEvent::RBM_ERROR, ex.what());
        }
    }

    /**
     * @brief Interrupt javascript execution
     */
    void MongoWorker::handle(StopScriptRequest *)
    {
        try {
            if (!_scriptEngine) {
                return;
            }

            _scriptEngine->interrupt();
        } catch(const std::exception &ex) {
            sendLog(this, LogEvent::RBM_ERROR, std::string(ex.what()));
        }
    }

    void MongoWorker::handle(AutocompleteRequest *event)
    {
        try {
            if (!_scriptEngine) {
                reply(event->sender(),
                    new AutocompleteResponse(this, EventError("MongoDB Shell was not initialized")));
                return;
            }

            QStringList list = _scriptEngine->complete(event->prefix, event->mode);
            reply(event->sender(), new AutocompleteResponse(this, list, event->prefix));
        } catch(const std::exception &ex) {
            reply(event->sender(), new AutocompleteResponse(this, EventError(ex.what())));
            sendLog(this, LogEvent::RBM_ERROR, std::string(ex.what()));
        }
    }

    void MongoWorker::handle(CreateDatabaseRequest *event)
    {
        std::string dbname = event->database();
        try {
            boost::scoped_ptr<MongoClient> client(getClient());
            client->createDatabase(dbname);

            // Insert to list of created database. Read docs for this hashset in the header
            _createdDbs.insert(dbname);

            reply(event->sender(), new CreateDatabaseResponse(this, dbname));
        } catch(const std::exception &ex) {
            reply(event->sender(), new CreateDatabaseResponse(this, dbname,
                EventError(ex.what()))
            );
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(DropDatabaseRequest *event)
    {
        try {
            boost::scoped_ptr<MongoClient> client(getClient());
            client->dropDatabase(event->database);

            // Remove from the list of created database, Read docs for this hashset in the header
            _createdDbs.erase(event->database);

            reply(event->sender(), new DropDatabaseResponse(this, event->database));
        }
        catch(const std::exception &ex) {
            reply(event->sender(),
                new DropDatabaseResponse(this, event->database, EventError(ex.what()))
            );
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(CreateCollectionRequest *event)
    {
        std::string const& collection = event->ns().collectionName();

        try {
            boost::scoped_ptr<MongoClient> client(getClient());
            client->createCollection(event->ns().toString(), event->getSize(), event->getCapped(),
                event->getMaxDocNum(), event->getExtraOptions());
            client->done();

            reply(event->sender(), new CreateCollectionResponse(this, collection));
        } catch(const std::exception &ex) {
            reply(event->sender(),
                new CreateCollectionResponse(this, collection, EventError(ex.what()))
            );
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(DropCollectionRequest *event)
    {
        std::string const& collection = event->ns().collectionName();

        try {
            boost::scoped_ptr<MongoClient> client(getClient());
            client->dropCollection(event->ns());
            client->done();

            reply(event->sender(), new DropCollectionResponse(this, collection));
        } catch(const std::exception &ex) {
            reply(event->sender(),
                new DropCollectionResponse(this, collection, EventError(ex.what()))
            );
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(RenameCollectionRequest *event)
    {
        try {
            boost::scoped_ptr<MongoClient> client(getClient());
            client->renameCollection(event->ns(), event->newCollection());
            client->done();

            reply(event->sender(), new RenameCollectionResponse(this, event->ns().collectionName(),
                                                                event->newCollection()));
        } catch(const std::exception &ex) {
            reply(event->sender(), new RenameCollectionResponse(this, EventError(ex.what())));
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(DuplicateCollectionRequest *event)
    {
        std::string const& sourceCollection = event->ns().collectionName();

        try {
            boost::scoped_ptr<MongoClient> client(getClient());
            client->duplicateCollection(event->ns(), event->newCollection());
            client->done();

            reply(event->sender(),
                new DuplicateCollectionResponse(this, sourceCollection, event->newCollection())
            );
        }
        catch (const std::exception &ex) {
            reply(event->sender(),
                new DuplicateCollectionResponse(this, sourceCollection, EventError(ex.what()))
            );
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(CopyCollectionToDiffServerRequest *event)
    {
        // Cross-server copy required two embedded driver connections; it was
        // already unused in the UI and is not supported by the mongosh layer.
        reply(event->sender(),
            new CopyCollectionToDiffServerResponse(this,
                EventError("Copy collection to a different server is not supported."))
        );
    }

    void MongoWorker::handle(CreateUserRequest *event)
    {
        try {
            boost::scoped_ptr<MongoClient> client(getClient());
            client->createUser(event->database(), event->user());
            client->done();

            reply(event->sender(), new CreateUserResponse(this, event->user().name()));
        } catch(const std::exception &ex) {
            reply(event->sender(),
                new CreateUserResponse(this, event->user().name(), EventError(ex.what()))
            );
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(DropUserRequest *event)
    {
        try {
            boost::scoped_ptr<MongoClient> client(getClient());
            client->dropUser(event->database(), event->username());
            client->done();

            reply(event->sender(), new DropUserResponse(this, event->username()));
        } catch(const std::exception &ex) {
            reply(event->sender(),
                new DropUserResponse(this, event->username(), EventError(ex.what()))
            );
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(CreateFunctionRequest *event)
    {
        std::string const& functionName = event->function().name();

        try {
            boost::scoped_ptr<MongoClient> client(getClient());
            client->createFunction(event->database(), event->function(), event->existingFunctionName());
            client->done();

            reply(event->sender(), new CreateFunctionResponse(this, functionName));
        } catch(const std::exception &ex) {
            reply(event->sender(),
                new CreateFunctionResponse(this, functionName, EventError(ex.what()))
            );
            // Logging handled in main thread
        }
    }

    void MongoWorker::handle(DropFunctionRequest *event)
    {
        try {
            boost::scoped_ptr<MongoClient> client(getClient());
            client->dropFunction(event->database(), event->functionName());
            client->done();

            reply(event->sender(), new DropFunctionResponse(this, event->functionName()));
        }
        catch (const std::exception &ex) {
            reply(event->sender(),
                new DropFunctionResponse(this, event->functionName(), EventError(ex.what()))
            );
            // Logging handled in main thread
        }
    }

    MongoClient *MongoWorker::getClient()
    {
        if (!_scriptEngine)
            init();
        return new MongoClient(_scriptEngine.get());
    }

    ReplicaSet MongoWorker::getReplicaSetInfo() const
    {
        std::string setName;
        mongo::HostAndPort primary;
        std::vector<std::pair<std::string, bool>> membersAndHealths;
        std::string errorStr;

        try {
            mongo::BSONObj status = _scriptEngine->evalCommand(
                "(() => { const s = rs.status(); return { set: s.set, "
                "members: (s.members || []).map(m => ({ name: m.name, "
                "health: m.health === 1, primary: m.stateStr === 'PRIMARY' })) }; })()",
                getAuthBase());

            setName = status.getStringField("set");
            for (auto const& memberElem : status.getField("members").Array()) {
                if (memberElem.type() != mongo::Object)
                    continue;
                mongo::BSONObj member = memberElem.Obj();
                std::string const name = member.getStringField("name");
                bool const health = member.getField("health").trueValue();
                membersAndHealths.push_back({ name, health });
                if (member.getField("primary").trueValue())
                    primary = mongo::HostAndPort(name);
            }
            if (primary.empty())
                errorStr = "Unable to determine primary member of replica set \"" + setName + "\".";
        }
        catch (const std::exception &ex) {
            errorStr = ex.what();
            // Report all configured members as unreachable
            for (auto const& member : _connSettings->replicaSetSettings()->members())
                membersAndHealths.push_back({ member, false });
        }

        return ReplicaSet(setName, primary, membersAndHealths, errorStr);
    }

    /**
     * @brief Send event to this MongoWorker
     */
    void MongoWorker::send(Event *event)
    {
        if (_isQuiting)
            return;

        AppRegistry::instance().bus()->send(this, event);
    }

    /**
     * @brief Send reply event to object 'receiver'
     */
    void MongoWorker::reply(QObject *receiver, Event *event)
    {
        if (_isQuiting)
            return;

        AppRegistry::instance().bus()->send(receiver, event);
    }
}
