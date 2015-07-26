/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2015  Vladimir Golovnev <glassez@yandex.ru>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 */

#include <QDebug>
#include <QCoreApplication>
#include <QVariant>
#include <QHash>
#include <QList>
#include <QScopedArrayPointer>
#include <QScopedPointer>
#include <QHostAddress>
#include <QDateTime>
#include <QFile>

#include "core/types.h"
#include "geoipdatabase.h"

struct Node
{
    quint32 left;
    quint32 right;
};

struct GeoIPData
{
    // Metadata
    quint16 ipVersion;
    quint16 recordSize;
    quint32 nodeCount;
    QDateTime buildEpoch;
    // Search data
    QList<Node> index;
    QHash<quint32, QString> countries;
};

namespace
{
    const quint32 __ENDIAN_TEST__ = 0x00000001;
    const bool __IS_LITTLE_ENDIAN__ = (reinterpret_cast<const uchar *>(&__ENDIAN_TEST__)[0] == 0x01);

    BEGIN_SCOPED_ENUM(DataType)
    {
        Unknown = 0,
        Pointer = 1,
        String = 2,
        Double = 3,
        Bytes = 4,
        Integer16 = 5,
        Integer32 = 6,
        Map = 7,
        SignedInteger32 = 8,
        Integer64 = 9,
        Integer128 = 10,
        Array = 11,
        DataCacheContainer = 12,
        EndMarker = 13,
        Boolean = 14,
        Float = 15
    }
    END_SCOPED_ENUM

    struct DataFieldDescriptor
    {
        DataType fieldType;
        union
        {
            quint32 fieldSize;
            quint32 offset; // Pointer
        };
    };

    const int MAX_FILE_SIZE = 10485760; // 10MB
    const char DB_TYPE[] = "GeoLite2-Country";

    const quint32 MAX_METADATA_SIZE = 131072; // 128KB
    const char METADATA_BEGIN_MARK[] = "\xab\xcd\xefMaxMind.com";
    const char DATA_SECTION_SEPARATOR[16] = { 0 };

#if (QT_VERSION < QT_VERSION_CHECK(5, 0, 0))
    Q_IPV6ADDR createMappedAddress(quint32 ip4);
#endif

    class Loader
    {
        Q_DECLARE_TR_FUNCTIONS(GeoIPDatabase)

    public:
        GeoIPData *load(const QString &filename);
        GeoIPData *load(const QByteArray &data);
        QString error() const;

    private:
        bool parseMetadata(const QVariantHash &metadata);
        bool loadDB();
        QVariantHash readMetadata();
        QVariant readDataField(quint32 &offset);
        bool readDataFieldDescriptor(quint32 &offset, DataFieldDescriptor &out);
        void fromBigEndian(uchar *buf, quint32 len);
        QVariant readMapValue(quint32 &offset, quint32 count);
        QVariant readArrayValue(quint32 &offset, quint32 count);

        template<typename T>
        QVariant readPlainValue(quint32 &offset, quint8 len)
        {
            T value = 0;
            const uchar *const data = m_data + offset;
            const quint32 availSize = m_size - offset;

            if ((len > 0) && (len <= sizeof(T) && (availSize >= len))) {
                // copy input data to last 'len' bytes of 'value'
                uchar *dst = reinterpret_cast<uchar *>(&value) + (sizeof(T) - len);
                memcpy(dst, data, len);
                fromBigEndian(reinterpret_cast<uchar *>(&value), sizeof(T));
                offset += len;
            }

            return QVariant::fromValue(value);
        }

    private:
        const uchar *m_data;
        quint32 m_size;
        QString m_error;
        GeoIPData *m_geoIPData;
    };
}

// GeoIPDatabase

GeoIPDatabase::GeoIPDatabase(GeoIPData *geoIPData)
    : m_geoIPData(geoIPData)
{
}

GeoIPDatabase *GeoIPDatabase::load(const QString &filename, QString &error)
{
    GeoIPDatabase *db = 0;

    Loader loader;
    GeoIPData *geoIPData = loader.load(filename);
    if (!geoIPData)
        error = loader.error();
    else
        db = new GeoIPDatabase(geoIPData);

    return db;
}

GeoIPDatabase *GeoIPDatabase::load(const QByteArray &data, QString &error)
{
    GeoIPDatabase *db = 0;

    Loader loader;
    GeoIPData *geoIPData = loader.load(data);
    if (!geoIPData)
        error = loader.error();
    else
        db = new GeoIPDatabase(geoIPData);

    return db;
}

GeoIPDatabase::~GeoIPDatabase()
{
    delete m_geoIPData;
}

QString GeoIPDatabase::type() const
{
    return DB_TYPE;
}

quint16 GeoIPDatabase::ipVersion() const
{
    return m_geoIPData->ipVersion;
}

QDateTime GeoIPDatabase::buildEpoch() const
{
    return m_geoIPData->buildEpoch;
}

QString GeoIPDatabase::lookup(const QHostAddress &hostAddr) const
{
#if (QT_VERSION < QT_VERSION_CHECK(5, 0, 0))
    Q_IPV6ADDR addr = hostAddr.protocol() == QAbstractSocket::IPv4Protocol
            ? createMappedAddress(hostAddr.toIPv4Address())
            : hostAddr.toIPv6Address();
#else
    Q_IPV6ADDR addr = hostAddr.toIPv6Address();
#endif
    const quint32 nodeCount = static_cast<quint32>(m_geoIPData->index.size());
    Node node = m_geoIPData->index[0];
    for (int i = 0; i < 16; ++i) {
        for (int j = 0; j < 8; ++j) {
            bool right = static_cast<bool>((addr[i] >> (7 - j)) & 1);
            quint32 id = (right ? node.right : node.left);
            if (id == nodeCount)
                return QString();
            else if (id > nodeCount)
                return m_geoIPData->countries[id];
            else
                node = m_geoIPData->index[id];
        }
    }

    return QString();
}

namespace
{
    // Loader

    GeoIPData *Loader::load(const QString &filename)
    {
        QFile file(filename);
        if (file.size() > MAX_FILE_SIZE) {
            m_error = tr("Unsupported database file size.");
            return 0;
        }

        if (!file.open(QFile::ReadOnly)) {
            m_error = file.errorString();
            return 0;
        }

        m_size = file.size();
        QScopedArrayPointer<uchar> data(new uchar[m_size]);
        m_data = data.data();
        if (file.read((char *)m_data, m_size) != m_size) {
            m_error = file.errorString();
            return 0;
        }

        QScopedPointer<GeoIPData> geoIPData(new GeoIPData);
        m_geoIPData = geoIPData.data();
        if (!parseMetadata(readMetadata()) || !loadDB())
            return 0;

        return geoIPData.take();
    }

    GeoIPData *Loader::load(const QByteArray &data)
    {
        if (data.size() > MAX_FILE_SIZE) {
            m_error = tr("Unsupported database file size.");
            return 0;
        }

        m_size = data.size();
        m_data = reinterpret_cast<const uchar *>(data.constData());

        QScopedPointer<GeoIPData> geoIPData(new GeoIPData);
        m_geoIPData = geoIPData.data();
        if (!parseMetadata(readMetadata()) || !loadDB())
            return 0;

        return geoIPData.take();
    }

    QString Loader::error() const
    {
        return m_error;
    }

#define CHECK_METADATA_REQ(key, type) \
    if (!metadata.contains(#key)) { \
        m_error = errMsgNotFound.arg(#key); \
        return false; \
    } \
    else if (metadata.value(#key).userType() != QMetaType::type) { \
        m_error = errMsgInvalid.arg(#key);  \
        return false; \
    }

#define CHECK_METADATA_OPT(key, type) \
    if (metadata.contains(#key)) { \
        if (metadata.value(#key).userType() != QMetaType::type) { \
            m_error = errMsgInvalid.arg(#key);  \
            return false; \
        } \
    }

    bool Loader::parseMetadata(const QVariantHash &metadata)
    {
        const QString errMsgNotFound = tr("Metadata error: '%1' entry not found.");
        const QString errMsgInvalid = tr("Metadata error: '%1' entry has invalid type.");

        qDebug() << "Parsing MaxMindDB metadata...";

        CHECK_METADATA_REQ(binary_format_major_version, UShort);
        CHECK_METADATA_REQ(binary_format_minor_version, UShort);
        uint versionMajor = metadata.value("binary_format_major_version").toUInt();
        uint versionMinor = metadata.value("binary_format_minor_version").toUInt();
        if (versionMajor != 2) {
            m_error = tr("Unsupported database version: %1.%2").arg(versionMajor).arg(versionMinor);
            return false;
        }

        CHECK_METADATA_REQ(ip_version, UShort);
        m_geoIPData->ipVersion = metadata.value("ip_version").value<quint16>();
        if (m_geoIPData->ipVersion != 6) {
            m_error = tr("Unsupported IP version: %1").arg(m_geoIPData->ipVersion);
            return false;
        }

        CHECK_METADATA_REQ(record_size, UShort);
        m_geoIPData->recordSize = metadata.value("record_size").value<quint16>();
        if (m_geoIPData->recordSize != 24) {
            m_error = tr("Unsupported record size: %1").arg(m_geoIPData->recordSize);
            return false;
        }

        CHECK_METADATA_REQ(node_count, UInt);
        m_geoIPData->nodeCount = metadata.value("node_count").value<quint32>();

        CHECK_METADATA_REQ(database_type, QString);
        QString dbType = metadata.value("database_type").toString();
        if (dbType != DB_TYPE) {
            m_error = tr("Invalid database type: %1").arg(dbType);
            return false;
        }

        CHECK_METADATA_REQ(build_epoch, ULongLong);
        m_geoIPData->buildEpoch = QDateTime::fromTime_t(metadata.value("build_epoch").toULongLong());

        CHECK_METADATA_OPT(languages, QVariantList);
        CHECK_METADATA_OPT(description, QVariantHash);

        return true;
    }

    bool Loader::loadDB()
    {
        qDebug() << "Parsing MaxMindDB index tree...";

        const int nodeSize = m_geoIPData->recordSize / 4; // in bytes
        const int indexSize = m_geoIPData->nodeCount * nodeSize;
        if ((m_size < (indexSize + sizeof(DATA_SECTION_SEPARATOR)))
            || (memcmp(m_data + indexSize, DATA_SECTION_SEPARATOR, sizeof(DATA_SECTION_SEPARATOR)) != 0)) {
            m_error = tr("Database corrupted: no data section found.");
            return false;
        }

        m_geoIPData->index.reserve(m_geoIPData->nodeCount);

        const int recordBytes = nodeSize / 2;
        const uchar *ptr = m_data;
        bool left = true;
        Node node;
        for (quint32 i = 0; i < (2 * m_geoIPData->nodeCount); ++i) {
            uchar buf[4] = { 0 };

            memcpy(&buf[4 - recordBytes], ptr, recordBytes);
            fromBigEndian(buf, 4);
            quint32 id = *(reinterpret_cast<quint32 *>(buf));

            if ((id > m_geoIPData->nodeCount) && !m_geoIPData->countries.contains(id)) {
                const quint32 offset = id - m_geoIPData->nodeCount - sizeof(DATA_SECTION_SEPARATOR);
                quint32 tmp = offset + indexSize + sizeof(DATA_SECTION_SEPARATOR);
                QVariant val = readDataField(tmp);
                if (val.userType() == QMetaType::QVariantHash) {
                    m_geoIPData->countries[id] = val.toHash()["country"].toHash()["iso_code"].toString();
                }
                else if (val.userType() == QVariant::Invalid) {
                    m_error = tr("Database corrupted: invalid data type at DATA@%1").arg(offset, 8, 16, QLatin1Char('0'));
                    return false;
                }
                else {
                    m_error = tr("Invalid database: unsupported data type at DATA@%1").arg(offset, 8, 16, QLatin1Char('0'));
                    return false;
                }
            }

            if (left) {
                node.left = id;
            }
            else {
                node.right = id;
                m_geoIPData->index << node;
            }

            left = !left;
            ptr += recordBytes;
        }

        return true;
    }

    QVariantHash Loader::readMetadata()
    {
        const char *ptr = reinterpret_cast<const char *>(m_data);
        quint32 size = m_size;
        if (m_size > MAX_METADATA_SIZE) {
            ptr += m_size - MAX_METADATA_SIZE;
            size = MAX_METADATA_SIZE;
        }

        const QByteArray data = QByteArray::fromRawData(ptr, size);
        int index = data.lastIndexOf(METADATA_BEGIN_MARK);
        if (index >= 0) {
            if (m_size > MAX_METADATA_SIZE)
                index += (m_size - MAX_METADATA_SIZE); // from begin of all data
            quint32 offset = static_cast<quint32>(index + strlen(METADATA_BEGIN_MARK));
            QVariant metadata = readDataField(offset);
            m_size = index; // truncate m_size to not contain metadata section
            if (metadata.userType() == QMetaType::QVariantHash)
                return metadata.toHash();
        }

        return QVariantHash();
    }

    QVariant Loader::readDataField(quint32 &offset)
    {
        DataFieldDescriptor descr;
        if (!readDataFieldDescriptor(offset, descr))
            return QVariant();

        quint32 locOffset = offset;
        bool usePointer = false;
        if (descr.fieldType == DataType::Pointer) {
            usePointer = true;
            // convert offset from data section to global
            locOffset = descr.offset + (m_geoIPData->nodeCount * m_geoIPData->recordSize / 4) + sizeof(DATA_SECTION_SEPARATOR);
            if (!readDataFieldDescriptor(locOffset, descr))
                return QVariant();
        }

        QVariant fieldValue;
        switch (descr.fieldType) {
        case DataType::Pointer:
            qDebug() << "* Illegal Pointer using";
            break;
        case DataType::String:
            fieldValue = QString::fromUtf8(reinterpret_cast<const char *>(m_data + locOffset), descr.fieldSize);
            locOffset += descr.fieldSize;
            break;
        case DataType::Double:
            if (descr.fieldSize == 8)
                fieldValue = readPlainValue<double>(locOffset, descr.fieldSize);
            else
                qDebug() << "* Invalid field size for type: Double";
            break;
        case DataType::Bytes:
            fieldValue = QByteArray(reinterpret_cast<const char *>(m_data + locOffset), descr.fieldSize);
            locOffset += descr.fieldSize;
            break;
        case DataType::Integer16:
            fieldValue = readPlainValue<quint16>(locOffset, descr.fieldSize);
            break;
        case DataType::Integer32:
            fieldValue = readPlainValue<quint32>(locOffset, descr.fieldSize);
            break;
        case DataType::Map:
            fieldValue = readMapValue(locOffset, descr.fieldSize);
            break;
        case DataType::SignedInteger32:
            fieldValue = readPlainValue<qint32>(locOffset, descr.fieldSize);
            break;
        case DataType::Integer64:
            fieldValue = readPlainValue<quint64>(locOffset, descr.fieldSize);
            break;
        case DataType::Integer128:
            qDebug() << "* Unsupported data type: Integer128";
            break;
        case DataType::Array:
            fieldValue = readArrayValue(locOffset, descr.fieldSize);
            break;
        case DataType::DataCacheContainer:
            qDebug() << "* Unsupported data type: DataCacheContainer";
            break;
        case DataType::EndMarker:
            qDebug() << "* Unsupported data type: EndMarker";
            break;
        case DataType::Boolean:
            fieldValue = QVariant::fromValue(static_cast<bool>(descr.fieldSize));
            break;
        case DataType::Float:
            if (descr.fieldSize == 4)
                fieldValue = readPlainValue<float>(locOffset, descr.fieldSize);
            else
                qDebug() << "* Invalid field size for type: Float";
            break;
        }

        if (!usePointer)
            offset = locOffset;
        return fieldValue;
    }

    bool Loader::readDataFieldDescriptor(quint32 &offset, DataFieldDescriptor &out)
    {
        const uchar *dataPtr = m_data + offset;
        int availSize = m_size - offset;
        if (availSize < 1) return false;

        out.fieldType = static_cast<DataType>((dataPtr[0] & 0xE0) >> 5);
        if (out.fieldType == DataType::Pointer) {
            int size = ((dataPtr[0] & 0x18) >> 3);
            if (availSize < (size + 2)) return false;

            if (size == 0)
                out.offset = ((dataPtr[0] & 0x07) << 8) + dataPtr[1];
            else if (size == 1)
                out.offset = ((dataPtr[0] & 0x07) << 16) + (dataPtr[1] << 8) + dataPtr[2] + 2048;
            else if (size == 2)
                out.offset = ((dataPtr[0] & 0x07) << 24) + (dataPtr[1] << 16) + (dataPtr[2] << 8) + dataPtr[3] + 526336;
            else if (size == 3)
                out.offset = (dataPtr[1] << 24) + (dataPtr[2] << 16) + (dataPtr[3] << 8) + dataPtr[4];

            offset += size + 2;
            return true;
        }

        out.fieldSize = dataPtr[0] & 0x1F;
        if (out.fieldSize <= 28) {
            if (out.fieldType == DataType::Unknown) {
                out.fieldType = static_cast<DataType>(dataPtr[1] + 7);
                if ((out.fieldType <= DataType::Map) || (out.fieldType > DataType::Float) || (availSize < 3))
                    return false;
                offset += 2;
            }
            else {
                offset += 1;
            }
        }
        else if (out.fieldSize == 29) {
            if (availSize < 2) return false;
            out.fieldSize = dataPtr[1] + 29;
            offset += 2;
        }
        else if (out.fieldSize == 30) {
            if (availSize < 3) return false;
            out.fieldSize = (dataPtr[1] << 8) + dataPtr[2] + 285;
            offset += 3;
        }
        else if (out.fieldSize == 31) {
            if (availSize < 4) return false;
            out.fieldSize = (dataPtr[1] << 16) + (dataPtr[2] << 8) + dataPtr[3] + 65821;
            offset += 4;
        }

        return true;
    }

    void Loader::fromBigEndian(uchar *buf, quint32 len)
    {
        if (__IS_LITTLE_ENDIAN__)
            std::reverse(buf, buf + len);
    }

    QVariant Loader::readMapValue(quint32 &offset, quint32 count)
    {
        QVariantHash map;

        for (quint32 i = 0; i < count; ++i) {
            QVariant field = readDataField(offset);
            if (field.userType() != QMetaType::QString)
                return QVariant();

            QString key = field.toString();
            field = readDataField(offset);
            if (field.userType() == QVariant::Invalid)
                return QVariant();

            map[key] = field;
        }

        return map;
    }

    QVariant Loader::readArrayValue(quint32 &offset, quint32 count)
    {
        QVariantList array;

        for (quint32 i = 0; i < count; ++i) {
            QVariant field = readDataField(offset);
            if (field.userType() == QVariant::Invalid)
                return QVariant();

            array.append(field);
        }

        return array;
    }

#if (QT_VERSION < QT_VERSION_CHECK(5, 0, 0))
    Q_IPV6ADDR createMappedAddress(quint32 ip4)
    {
        Q_IPV6ADDR ip6;
        memset(&ip6, 0, sizeof(ip6));

        int i;
        for (i = 15; ip4 != 0; i--) {
            ip6[i] = ip4 & 0xFF;
            ip4 >>= 8;
        }

        ip6[11] = 0xFF;
        ip6[10] = 0xFF;

        return ip6;
    }
#endif
}
