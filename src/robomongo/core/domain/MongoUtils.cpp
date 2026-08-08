#include "robomongo/core/domain/MongoUtils.h"

#include <QCryptographicHash>

namespace Robomongo
{
    namespace MongoUtils
    {
        QString buildNiceSizeString(double sizeBytes)
        {
            if (sizeBytes < 1024 * 100) {
                double kb = ((double) sizeBytes) / 1024;
                return QString("%1 kb").arg(kb, 2, 'f', 2);
            }

            double mb = ((double) sizeBytes) / 1024 / 1024;
            return QString("%1 mb").arg(mb, 2, 'f', 2);
        }

        std::string buildPasswordHash(const std::string &username, const std::string &password)
        {
            // Legacy MONGODB-CR digest: md5(username + ":mongo:" + password)
            const std::string sum = username + ":mongo:" + password;
            const QByteArray digest = QCryptographicHash::hash(
                QByteArray(sum.data(), static_cast<int>(sum.size())),
                QCryptographicHash::Md5);
            return digest.toHex().toStdString();
        }
    }
}
