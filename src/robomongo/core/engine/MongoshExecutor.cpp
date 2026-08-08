#include "robomongo/core/engine/MongoshExecutor.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QUrl>

#include <QDateTime>
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
