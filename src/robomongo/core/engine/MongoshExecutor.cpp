#include "robomongo/core/engine/MongoshExecutor.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QUrl>

#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>

#include "robomongo/core/settings/ConnectionSettings.h"
#include "robomongo/core/settings/CredentialSettings.h"
#include "robomongo/core/settings/ReplicaSetSettings.h"
#include "robomongo/core/settings/SslSettings.h"

namespace Robomongo
{
    namespace
    {
        // Hard ceiling applied when no positive shell timeout is configured,
        // so a stalled mongosh cannot hang a worker thread indefinitely.
        constexpr int kNoTimeoutCeilingSec = 120;

        bool debugLoggingEnabled()
        {
            static const bool enabled =
                !qEnvironmentVariableIsEmpty("ROBO_MONGOSH_DEBUG");
            return enabled;
        }

        void writeDebug(const QString &line)
        {
            if (!debugLoggingEnabled())
                return;
            QFile f(QDir(QDir::tempPath()).filePath("robo-mongosh.log"));
            if (f.open(QIODevice::Append | QIODevice::Text)) {
                QTextStream(&f) << QDateTime::currentDateTime().toString(Qt::ISODate)
                                << " " << line << "\n";
            }
        }

        // Redacts the password that follows -p / --password so it never
        // reaches the log file.
        QStringList redactArgs(const QStringList &args)
        {
            QStringList out = args;
            for (int i = 0; i < out.size() - 1; ++i)
                if (out[i] == "-p" || out[i] == "--password")
                    out[i + 1] = "***";
            return out;
        }

        void logInvocation(const QString &binary, const QStringList &args)
        {
            writeDebug("EXEC " + binary + " " + redactArgs(args).join(" "));
        }

        void logResult(const MongoshExecutor::EvalResult &r)
        {
            if (!debugLoggingEnabled())
                return;
            QString summary = QString("DONE exit=%1 timedOut=%2 failedToStart=%3 "
                                      "hasResult=%4 isError=%5")
                                  .arg(r.exitCode).arg(r.timedOut).arg(r.failedToStart)
                                  .arg(r.output.hasResult).arg(r.output.isError);
            writeDebug(summary);
            if (!r.output.errorMessage.empty())
                writeDebug("  errorMessage: " + QString::fromStdString(r.output.errorMessage));
            if (!r.errorOutput.empty())
                writeDebug("  stderr: " +
                           QString::fromStdString(r.errorOutput.substr(0, 2000)));
            if (!r.output.text.empty())
                writeDebug("  stdout-text: " +
                           QString::fromStdString(r.output.text.substr(0, 500)));
        }
    }
    QString MongoshExecutor::findMongoshBinary()
    {
#ifdef Q_OS_WIN
        const QString binaryName = "mongosh.exe";
#else
        const QString binaryName = "mongosh";
#endif

#ifdef Q_OS_MAC
        // 1. Bundled: Contents/Helpers (outside Contents/MacOS, so
        // LaunchServices does not treat each spawn as an app launch -
        // that would bounce a Dock icon per evaluation)
        const QString helpers = QDir(QCoreApplication::applicationDirPath())
                                    .filePath("../Helpers/" + binaryName);
        if (QFileInfo::exists(helpers))
            return QFileInfo(helpers).canonicalFilePath();
#endif

        // Bundled next to the application executable (Windows/Linux;
        // also the historical macOS location)
        const QString bundled =
            QDir(QCoreApplication::applicationDirPath()).filePath(binaryName);
        if (QFileInfo::exists(bundled))
            return bundled;

        // 2. Explicit override (development)
        const QString overridePath =
            QProcessEnvironment::systemEnvironment().value("ROBO_MONGOSH_PATH");
        if (!overridePath.isEmpty() && QFileInfo::exists(overridePath))
            return overridePath;

        // 3. PATH
        return QStandardPaths::findExecutable(binaryName);
    }

    QStringList MongoshExecutor::connectionArgs(ConnectionSettings *connection,
                                                const std::string &serverAddr,
                                                const std::string &dbName)
    {
        QStringList args;

        std::string hostPort = serverAddr.empty()
            ? connection->serverHost() + ":" + std::to_string(connection->serverPort())
            : serverAddr;

        // Replica set: connect with all members (and set name when known)
        std::string replicaSetName;
        if (connection->isReplicaSet() && serverAddr.empty()) {
            ReplicaSetSettings *rs = connection->replicaSetSettings();
            const std::vector<std::string> &members = rs->members();
            if (!members.empty()) {
                hostPort.clear();
                for (size_t i = 0; i < members.size(); ++i) {
                    if (i) hostPort += ",";
                    hostPort += members[i];
                }
            }
            replicaSetName = rs->setNameUserEntered().empty() ? rs->cachedSetName()
                                                              : rs->setNameUserEntered();
        }

        std::string database = dbName;
        if (database.empty())
            database = connection->defaultDatabase().empty() ? "test"
                                                             : connection->defaultDatabase();

        // Connection string: mongodb://host:port/db . Credentials are passed
        // as separate arguments (never percent-encoded into the URI, and
        // never visible in `ps` output longer than necessary - a TODO here
        // is to move the password to MONGOSH_* env or stdin prompt).
        QString uri = QString("mongodb://%1/%2")
                          .arg(QString::fromStdString(hostPort),
                               QString::fromStdString(QUrl::toPercentEncoding(
                                   QString::fromStdString(database)).toStdString()));

        // Direct connection: Robo addresses specific hosts itself (and via
        // SSH tunnels the remote is not aware of the tunnel address)
        if (!connection->isReplicaSet() || !serverAddr.empty())
            uri += "?directConnection=true";
        else if (!replicaSetName.empty())
            uri += "?replicaSet=" + QString::fromStdString(QUrl::toPercentEncoding(
                       QString::fromStdString(replicaSetName)).toStdString());

        args << uri;

        if (connection->hasEnabledPrimaryCredential()) {
            CredentialSettings *cred = connection->primaryCredential();
            args << "-u" << QString::fromStdString(cred->userName());
            args << "-p" << QString::fromStdString(cred->userPassword());
            args << "--authenticationDatabase" << QString::fromStdString(cred->databaseName());
            const std::string mechanism = cred->mechanism();
            // mongosh negotiates SCRAM automatically; only pass explicit
            // non-default mechanisms
            if (!mechanism.empty() && mechanism != "SCRAM-SHA-1" && mechanism != "MONGODB-CR")
                args << "--authenticationMechanism" << QString::fromStdString(mechanism);
        }

        SslSettings *ssl = connection->sslSettings();
        if (ssl && ssl->sslEnabled()) {
            args << "--tls";
            if (!ssl->caFile().empty())
                args << "--tlsCAFile" << QString::fromStdString(ssl->caFile());
            if (ssl->usePemFile() && !ssl->pemKeyFile().empty())
                args << "--tlsCertificateKeyFile" << QString::fromStdString(ssl->pemKeyFile());
            if (!ssl->pemPassPhrase().empty())
                args << "--tlsCertificateKeyFilePassword"
                     << QString::fromStdString(ssl->pemPassPhrase());
            if (ssl->allowInvalidCertificates())
                args << "--tlsAllowInvalidCertificates";
            if (ssl->allowInvalidHostnames())
                args << "--tlsAllowInvalidHostnames";
            if (!ssl->crlFile().empty())
                args << "--tlsCRLFile" << QString::fromStdString(ssl->crlFile());
        }

        return args;
    }

    MongoshExecutor::EvalResult MongoshExecutor::eval(const QStringList &connArgs,
                                                      const std::vector<std::string> &scripts,
                                                      int timeoutSec)
    {
        EvalResult result;

        const QString binary = findMongoshBinary();
        if (binary.isEmpty()) {
            result.failedToStart = true;
            result.errorOutput =
                "mongosh binary not found. It should be located next to the Robo 3T "
                "executable (bundled by the installer). For development builds set "
                "ROBO_MONGOSH_PATH or add mongosh to PATH.";
            return result;
        }

        QStringList args = connArgs;
        args << "--quiet" << "--norc" << "--json=canonical";
        for (const std::string &script : scripts)
            args << "--eval" << QString::fromStdString(script);

        QProcess process;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("NO_COLOR", "1");
        process.setProcessEnvironment(env);

        logInvocation(binary, args);

        _activeProcess.store(&process);
        process.start(binary, args);

        if (!process.waitForStarted(15000)) {
            _activeProcess.store(nullptr);
            result.failedToStart = true;
            result.errorOutput = "Failed to start mongosh: " +
                                 process.errorString().toStdString();
            logResult(result);
            return result;
        }

        // Close mongosh's stdin so any interactive prompt (password, OIDC
        // device-auth, confirmation) gets EOF and fails fast, instead of
        // blocking the worker thread forever waiting on input that never
        // comes (QProcess keeps the write channel open otherwise).
        process.closeWriteChannel();

        // Never wait forever. timeoutSec<=0 means "no user shell timeout",
        // but we still cap at a hard ceiling so a stalled mongosh (network
        // black hole, unexpected prompt) surfaces as an error rather than
        // an indefinite spinner.
        const int effectiveSec = timeoutSec > 0 ? timeoutSec : kNoTimeoutCeilingSec;
        if (!process.waitForFinished(effectiveSec * 1000)) {
            result.timedOut = true;
            process.kill();
            process.waitForFinished(5000);
        }
        _activeProcess.store(nullptr);

        result.exitCode = process.exitCode();
        result.errorOutput += process.readAllStandardError().toStdString();
        result.output = parseMongoshOutput(process.readAllStandardOutput().toStdString());

        // Killed by interrupt() rather than a natural finish
        if (process.exitStatus() == QProcess::CrashExit && !result.timedOut &&
            result.output.text.empty() && !result.output.hasResult) {
            result.output.text = "Script execution interrupted.";
        }

        logResult(result);
        return result;
    }

    void MongoshExecutor::interrupt()
    {
        if (QProcess *p = _activeProcess.load())
            p->kill();
    }
}

namespace Robomongo
{
    namespace
    {
        /**
         * Session prelude, sent once per process over stdin (single line -
         * the REPL evaluates line by line). Silences the prompt, patches
         * Collection.find/aggregate to record paging metadata (cursor
         * methods are async-rewriter proxies and must not be touched), and
         * defines __roboRun which evaluates a script, materializes cursors
         * and prints a sentinel-framed EJSON envelope.
         */
        const char *kSessionPrelude =
            "prompt = () => \"\";"
            " globalThis.__robo = { queryInfo: null, aggrInfo: null };"
            " try { const __rc = db.getCollection(\"__robo_probe__\");"
            " const __rp = Object.getPrototypeOf(__rc);"
            " const __rf = __rp.find;"
            " __rp.find = function (f, p, o) {"
            "   try { globalThis.__robo.queryInfo = {"
            "     db: (this._database && this._database.getName) ? this._database.getName() : db.getName(),"
            "     collection: this.getName ? this.getName() : String(this._name),"
            "     query: f || {}, fields: p || {}, limit: 0, skip: 0, batchSize: 0 }; } catch (e) {}"
            "   return __rf.call(this, f, p, o); };"
            " const __ra = __rp.aggregate;"
            " __rp.aggregate = function (pl, o) {"
            "   try { globalThis.__robo.aggrInfo = {"
            "     collection: this.getName ? this.getName() : String(this._name),"
            "     pipeline: pl || [], options: o || {} }; } catch (e) {}"
            "   return __ra.apply(this, arguments); }; } catch (e) {}"
            " globalThis.__roboCheck = function (id, src) {"
            "   const out = { ok: true };"
            "   try { new Function(src); }"
            "   catch (e) { out.ok = false; out.error = {"
            "     name: (e && e.name) || \"SyntaxError\","
            "     message: (e && e.message) || \"Syntax error\" }; }"
            "   print(\"\\n__RB_\" + id + \"__\" + JSON.stringify(out) + \"__RE_\" + id + \"__\");"
            " };"
            " globalThis.__roboEmit = async function (id, src) {"
            "   const out = { ok: true };"
            "   try {"
            "     let v = globalThis.__robo_last;"
            "     if (v && typeof v.then === \"function\") v = await v;"
            "     if (v && typeof v.toArray === \"function\" && typeof v.hasNext === \"function\") {"
            "       const d = [];"
            "       while (d.length < 1000 && await v.hasNext()) d.push(await v.next());"
            "       v = d; }"
            "     out.hasResult = v !== undefined; out.result = v === undefined ? null : v;"
            "   } catch (e) { out.ok = false; out.error = {"
            "     name: (e && e.name) || \"Error\","
            "     message: (e && (e.message || String(e))) || \"Error\" }; }"
            "   if (globalThis.__robo.queryInfo && src) { try {"
            "     const qi = globalThis.__robo.queryInfo;"
            "     const lm = src.match(/\\.limit\\(\\s*(\\d+)/); if (lm) qi.limit = parseInt(lm[1]);"
            "     const sm = src.match(/\\.skip\\(\\s*(\\d+)/); if (sm) qi.skip = parseInt(sm[1]);"
            "   } catch (e) {} }"
            "   const safe = (fn) => { try { return fn(); } catch (e) { return null; } };"
            "   out.meta = { server: safe(() => db.getMongo()._uri),"
            "     db: safe(() => db.getName()),"
            "     queryInfo: globalThis.__robo.queryInfo, aggrInfo: globalThis.__robo.aggrInfo };"
            "   let pl;"
            "   try { pl = EJSON.stringify(out, { relaxed: false }); }"
            "   catch (e) { pl = EJSON.stringify({ ok: false, error: { name: \"BSONError\","
            "     message: \"Result is not serializable: \" + e.message }, meta: out.meta },"
            "     { relaxed: false }); }"
            "   print(\"\\n__RB_\" + id + \"__\" + pl + \"__RE_\" + id + \"__\");"
            " };"
            " globalThis.__roboRun = async function (id, src, dbName) {"
            "   const out = { ok: true };"
            "   globalThis.__robo.queryInfo = null; globalThis.__robo.aggrInfo = null;"
            "   try {"
            "     if (dbName) db = db.getSiblingDB(dbName);"
            "     let v = eval(src);"
            "     if (v && typeof v.then === \"function\") v = await v;"
            "     if (v && typeof v.toArray === \"function\" && typeof v.hasNext === \"function\") {"
            "       const d = [];"
            "       while (d.length < 1000 && await v.hasNext()) d.push(await v.next());"
            "       v = d; }"
            "     out.hasResult = v !== undefined; out.result = v === undefined ? null : v;"
            "   } catch (e) { out.ok = false; out.error = {"
            "     name: (e && e.name) || \"Error\","
            "     message: (e && (e.message || String(e))) || \"Error\" }; }"
            "   if (globalThis.__robo.queryInfo) { try {"
            "     const qi = globalThis.__robo.queryInfo;"
            "     const lm = src.match(/\\.limit\\(\\s*(\\d+)/); if (lm) qi.limit = parseInt(lm[1]);"
            "     const sm = src.match(/\\.skip\\(\\s*(\\d+)/); if (sm) qi.skip = parseInt(sm[1]);"
            "   } catch (e) {} }"
            "   const safe = (fn) => { try { return fn(); } catch (e) { return null; } };"
            "   out.meta = { server: safe(() => db.getMongo()._uri),"
            "     db: safe(() => db.getName()),"
            "     queryInfo: globalThis.__robo.queryInfo, aggrInfo: globalThis.__robo.aggrInfo };"
            "   let pl;"
            "   try { pl = EJSON.stringify(out, { relaxed: false }); }"
            "   catch (e) { pl = EJSON.stringify({ ok: false, error: { name: \"BSONError\","
            "     message: \"Result is not serializable: \" + e.message }, meta: out.meta },"
            "     { relaxed: false }); }"
            "   print(\"\\n__RB_\" + id + \"__\" + pl + \"__RE_\" + id + \"__\");"
            " };";
    }

    MongoshSession::~MongoshSession()
    {
        shutdown();
    }

    void MongoshSession::shutdown()
    {
        if (_process) {
            _process->kill();
            _process->waitForFinished(3000);
            _process.reset();
        }
    }

    void MongoshSession::interrupt()
    {
        if (_process)
            _process->kill();
    }

    void MongoshSession::readEnvelope(const std::string &id, int effectiveSec,
                                      QElapsedTimer &timer, RunOutcome &outcome)
    {
        const std::string beginMarker = "__RB_" + id + "__";
        const std::string endMarker = "__RE_" + id + "__";

        // Read until the end marker, the deadline, or process death
        std::string buffer;
        while (buffer.find(endMarker) == std::string::npos) {
            if (timer.elapsed() > effectiveSec * 1000LL) {
                outcome.timedOut = true;
                shutdown();
                return;
            }
            _process->waitForReadyRead(200);
            buffer += _process->readAllStandardOutput().toStdString();
            if (_process->state() != QProcess::Running &&
                buffer.find(endMarker) == std::string::npos) {
                buffer += _process->readAllStandardOutput().toStdString();
                if (buffer.find(endMarker) != std::string::npos)
                    break;
                outcome.sessionError = true;
                outcome.sessionErrorMessage =
                    "mongosh session ended unexpectedly. " +
                    _process->readAllStandardError().toStdString();
                shutdown();
                return;
            }
        }

        const size_t beginPos = buffer.find(beginMarker);
        const size_t endPos = buffer.find(endMarker);
        if (beginPos == std::string::npos || endPos <= beginPos) {
            outcome.sessionError = true;
            outcome.sessionErrorMessage = "Malformed mongosh session response.";
            shutdown();
            return;
        }

        // Text printed before the envelope (user print()s, helper output)
        std::string pre = buffer.substr(0, beginPos);
        while (!pre.empty() && (pre.back() == '\n' || pre.back() == '\r'))
            pre.pop_back();
        outcome.textOutput = pre;

        try {
            const std::string payload =
                buffer.substr(beginPos + beginMarker.size(),
                              endPos - beginPos - beginMarker.size());
            mongo::BSONObj envelope = mongo::fromjson(payload);

            outcome.ok = envelope.getField("ok").trueValue();
            if (!outcome.ok) {
                mongo::BSONObj err = envelope.getObjectField("error");
                outcome.errorName = err.getStringField("name");
                outcome.errorMessage = err.getStringField("message");
            }

            mongo::BSONElement result = envelope.getField("result");
            if (envelope.getField("hasResult").trueValue() && !result.isNull()) {
                if (result.type() == mongo::Object || result.type() == mongo::Array) {
                    outcome.hasResult = true;
                    outcome.result = result.Obj();
                } else if (!result.eoo()) {
                    outcome.scalarResult = true;
                    outcome.scalarText = result.toString(false);
                }
            }

            mongo::BSONElement meta = envelope.getField("meta");
            if (meta.type() == mongo::Object) {
                outcome.meta = meta.Obj();
                outcome.hasMeta = true;
            }
        } catch (const std::exception &e) {
            outcome.sessionError = true;
            outcome.sessionErrorMessage =
                std::string("Failed to parse mongosh session response: ") + e.what();
        }
    }

    bool MongoshSession::ensureStarted(const QStringList &connArgs, std::string *error)
    {
        if (_process && _process->state() == QProcess::Running && _connArgs == connArgs)
            return true;

        shutdown();

        const QString binary = MongoshExecutor::findMongoshBinary();
        if (binary.isEmpty()) {
            *error = "mongosh binary not found. It should be located next to the Robo 3T "
                     "executable (bundled by the installer). For development builds set "
                     "ROBO_MONGOSH_PATH or add mongosh to PATH.";
            return false;
        }

        QStringList args = connArgs;
        args << "--quiet" << "--norc";

        _process.reset(new QProcess);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("NO_COLOR", "1");
        _process->setProcessEnvironment(env);

        logInvocation(binary, redactArgs(args));
        _process->start(binary, args);

        if (!_process->waitForStarted(15000)) {
            *error = "Failed to start mongosh: " + _process->errorString().toStdString();
            _process.reset();
            return false;
        }

        _process->write(kSessionPrelude);
        _process->write("\n");
        _connArgs = connArgs;
        return true;
    }

    MongoshSession::RunOutcome MongoshSession::run(const QStringList &connArgs,
                                                   const std::string &script,
                                                   const std::string &dbName,
                                                   Mode mode, int timeoutSec)
    {
        RunOutcome outcome;

        std::string startError;
        if (!ensureStarted(connArgs, &startError)) {
            outcome.sessionError = true;
            outcome.sessionErrorMessage = startError;
            return outcome;
        }

        const std::string id = std::to_string(++_sequence);
        const std::string beginMarker = "__RB_" + id + "__";
        const std::string endMarker = "__RE_" + id + "__";

        if (mode == Mode::Internal) {
            // Envelope-captured eval: errors become envelope errors
            std::string command =
                "__roboRun(\"" + id + "\", \"" + mongo::str::escape(script) + "\"";
            if (!dbName.empty())
                command += ", \"" + mongo::str::escape(dbName) + "\"";
            command += ")\n";
            writeDebug("RUN " + QString::fromStdString(
                command.size() > 300 ? command.substr(0, 300) + "..." : command));
            _process->write(command.c_str());
        } else {
            // Raw REPL input: mongosh's async rewriting applies. Reset the
            // captured value and paging metadata, switch db, evaluate the
            // script void-assigned (suppresses the REPL echo), then emit
            // the envelope. A helper line (use/show/...) goes verbatim.
            _process->write("void (globalThis.__robo_last = undefined,"
                            " globalThis.__robo.queryInfo = null,"
                            " globalThis.__robo.aggrInfo = null)\n");
            if (!dbName.empty())
                _process->write(("void (db = db.getSiblingDB(\"" +
                                 mongo::str::escape(dbName) + "\"))\n").c_str());
            if (mode == Mode::Helper) {
                _process->write(script.c_str());
                _process->write("\n");
            } else {
                // Trailing semicolons/whitespace would break the wrapping
                // parentheses (`(expr;)` is a syntax error)
                std::string trimmed = script;
                while (!trimmed.empty() &&
                       (std::isspace(static_cast<unsigned char>(trimmed.back())) ||
                        trimmed.back() == ';'))
                    trimmed.pop_back();
                _process->write("void (globalThis.__robo_last = (\n");
                _process->write(trimmed.c_str());
                _process->write("\n))\n");
            }
            const std::string emitLine =
                "__roboEmit(\"" + id + "\", \"" + mongo::str::escape(script) + "\")\n";
            writeDebug("RAW-RUN " + QString::fromStdString(
                script.size() > 300 ? script.substr(0, 300) + "..." : script));
            _process->write(emitLine.c_str());
        }

        const int effectiveSec = timeoutSec > 0 ? timeoutSec : 120;
        QElapsedTimer timer;
        timer.start();
        readEnvelope(id, effectiveSec, timer, outcome);
        if (outcome.timedOut || outcome.sessionError)
            return outcome;

        // Multi-statement user scripts cannot be wrapped in the
        // void-assignment parentheses (SyntaxError before evaluation).
        // If the script parses as a standalone program, re-run it as plain
        // REPL input: statements execute with full mongosh semantics and
        // the REPL's echo of the final value becomes the text output.
        // The parse check is essential: raw input with unbalanced braces
        // would put the REPL into multiline continuation and swallow every
        // subsequent command until the timeout.
        if (mode == Mode::UserScript && outcome.ok && !outcome.hasResult &&
            !outcome.scalarResult &&
            outcome.textOutput.find("SyntaxError") != std::string::npos &&
            outcome.textOutput.find("__robo_last = (") != std::string::npos) {
            RunOutcome check;
            const std::string checkId = std::to_string(++_sequence);
            _process->write(("__roboCheck(\"" + checkId + "\", \"" +
                             mongo::str::escape(script) + "\")\n").c_str());
            readEnvelope(checkId, effectiveSec, timer, check);
            if (check.timedOut || check.sessionError)
                return check;
            if (!check.ok) {
                // Genuine syntax error in the user script - surface it as
                // a proper error result
                check.hasMeta = outcome.hasMeta;
                check.meta = outcome.meta;
                return check;
            }

            RunOutcome retry;
            const std::string retryId = std::to_string(++_sequence);
            _process->write(script.c_str());
            _process->write("\n");
            _process->write(("__roboEmit(\"" + retryId + "\", \"" +
                             mongo::str::escape(script) + "\")\n").c_str());
            readEnvelope(retryId, effectiveSec, timer, retry);
            return retry;
        }

        return outcome;
    }
}
