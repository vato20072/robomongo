#include "robomongo/core/engine/MongoshExecutor.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QUrl>

#include "robomongo/core/settings/ConnectionSettings.h"
#include "robomongo/core/settings/CredentialSettings.h"
#include "robomongo/core/settings/SslSettings.h"

namespace Robomongo
{
    QString MongoshExecutor::findMongoshBinary()
    {
#ifdef Q_OS_WIN
        const QString binaryName = "mongosh.exe";
#else
        const QString binaryName = "mongosh";
#endif

        // 1. Bundled: next to the application executable
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
        if (!connection->isReplicaSet())
            uri += "?directConnection=true";

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

        _activeProcess.store(&process);
        process.start(binary, args);

        if (!process.waitForStarted(15000)) {
            _activeProcess.store(nullptr);
            result.failedToStart = true;
            result.errorOutput = "Failed to start mongosh: " +
                                 process.errorString().toStdString();
            return result;
        }

        const int timeoutMs = timeoutSec > 0 ? timeoutSec * 1000 : -1;
        if (!process.waitForFinished(timeoutMs)) {
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

        return result;
    }

    void MongoshExecutor::interrupt()
    {
        if (QProcess *p = _activeProcess.load())
            p->kill();
    }
}
