/* This file is part of the KDE project
   Copyright (C) 2003 Adam Pigg <adam@piggz.co.uk>
   Copyright (C) 2010 Jarosław Staniek <staniek@kde.org>

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public License
   along with this program; see the file COPYING.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
*/

#include "PostgresqlCursor.h"
#include "PostgresqlConnection.h"
#include "PostgresqlConnection_p.h"
#include "PostgresqlDriver.h"

#include "KDbError.h"
#include "KDbGlobal.h"



// Constructor based on query statement
PostgresqlCursor::PostgresqlCursor(KDbConnection* conn, const KDbEscapedString& sql, int options)
        : KDbCursor(conn, sql, options)
        , m_numRows(0)
        , d(new PostgresqlCursorData(conn))
{
    m_options |= Buffered;
}

//==================================================================================
//Constructor base on query object
PostgresqlCursor::PostgresqlCursor(KDbConnection* conn, KDbQuerySchema* query, int options)
        : KDbCursor(conn, query, options)
        , d(new PostgresqlCursorData(conn))
{
    m_options |= Buffered;
}

//==================================================================================
//Destructor
PostgresqlCursor::~PostgresqlCursor()
{
    close();
    delete d;
}


//==================================================================================
//Create a cursor result set
bool PostgresqlCursor::drv_open(const KDbEscapedString& sql)
{
    if (!d->executeSQL(sql, PGRES_TUPLES_OK))
        return false;

    m_fieldsToStoreInRecord = PQnfields(d->res);
    m_fieldCount = m_fieldsToStoreInRecord - (m_containsRecordIdInfo ? 1 : 0);
    m_numRows = PQntuples(d->res);
    m_records_in_buf = m_numRows;
    m_buffering_completed = true;

    // get real types for all fields
    PostgresqlDriver* drv = static_cast<PostgresqlDriver*>(m_conn->driver());

    m_realTypes.resize(m_fieldsToStoreInRecord);
    for (int i = 0; i < int(m_fieldsToStoreInRecord); i++) {
        const int pqtype = PQftype(d->res, i);
        m_realTypes[i] = drv->pgsqlToVariantType(pqtype);
    }
    return true;
}

//==================================================================================
//Delete objects
bool PostgresqlCursor::drv_close()
{
    PQclear(d->res);
    return true;
}

//==================================================================================
//Gets the next record...does not need to do much, just return fetchend if at end of result set
void PostgresqlCursor::drv_getNextRecord()
{
    if (at() >= qint64(m_numRows)) {
        m_fetchResult = FetchEnd;
    }
    else if (at() < 0) {
        // control will reach here only when at() < 0 ( which is usually -1 )
        // -1 is same as "1 beyond the End"
        m_fetchResult = FetchEnd;
    }
    else { // 0 <= at() < m_numRows
        m_fetchResult = FetchOK;
    }
}

//==================================================================================
//Check the current position is within boundaries
#if 0
void PostgresqlCursor::drv_getPrevRecord()
{
// KDbDrvDbg;

    if (at() < m_res->size() && at() >= 0) {
        m_fetchResult = FetchOK;
    } else if (at() >= m_res->size()) {
        m_fetchResult = FetchEnd;
    } else {
        m_fetchResult = FetchError;
    }
}
#endif

//==================================================================================
//Return the value for a given column for the current record
QVariant PostgresqlCursor::value(int pos)
{
    if (pos < m_fieldCount)
        return pValue(pos);
    else
        return QVariant();
}

#if 0
inline QVariant pgsqlCStrToVariant(const pqxx::result::field& r)
{
    switch (r.type()) {
    case BOOLOID:
        return QString::fromLatin1(r.c_str(), r.size()) == "true"; //!< @todo check formatting
    case INT2OID:
    case INT4OID:
    case INT8OID:
        return r.as(int());
    case FLOAT4OID:
    case FLOAT8OID:
    case NUMERICOID:
        return r.as(double());
    case DATEOID:
        return QString::fromUtf8(r.c_str(), r.size()); //!< @todo check formatting
    case TIMEOID:
        return QString::fromUtf8(r.c_str(), r.size()); //!< @todo check formatting
    case TIMESTAMPOID:
        return QString::fromUtf8(r.c_str(), r.size()); //!< @todo check formatting
    case BYTEAOID:
        return KDb::pgsqlByteaToByteArray(r.c_str(), r.size());
    case BPCHAROID:
    case VARCHAROID:
    case TEXTOID:
        return QString::fromUtf8(r.c_str(), r.size()); //utf8?
    default:
        return QString::fromUtf8(r.c_str(), r.size()); //utf8?
    }
}
#endif

static inline bool hasTimeZone(const QString& s)
{
    return s.at(s.length() - 3) == QLatin1Char('+') || s.at(s.length() - 3) == QLatin1Char('-');
}

static inline QVariant convertToKDbType(bool convert, const QVariant &value, QVariant::Type kdbVariantType)
{
    return (convert && value.canConvert(kdbVariantType)) ? value.value<kdbVariantType>() : value;
}

static inline QTime timeFromData(const char *data, int len)
{
    if (len == 0) {
        return QTime();
    }
    QString s(QString::fromLatin1(data, len));
    if (hasTimeZone(s)) {
        s.chop(3); // skip timezone
        return QTime::fromString(s, Qt::ISODate);
    }
    return QTime::fromString(s, Qt::ISODate);
}

static inline QDateTime dateTimeFromData(const char *data, int len)
{
    if (len < 10 /*ISO Date*/) {
        return QDateTime();
    }
    QString s(QString::fromLatin1(data, len));
    if (hasTimeZone(s)) {
        s.chop(3); // skip timezone
        if (s.isEmpty()) {
            return QDateTime();
        }
    }
    if (s.at(s.length() - 3).isPunct()) { // fix ms, should be three digits
        s += QLatin1Char('0');
    }
    return QDateTime::fromString(s, Qt::ISODate);
}

static inline QDateTime byteArrayFromData(const char *data)
{
    size_t unescapedLen;
    unsigned char *unescapedData = PQunescapeBytea((const unsigned char*)data, &unescapedLen);
    const QByteArray result((const char*)unescapedData, unescapedLen);
    //! @todo avoid deep copy; QByteArray does not allow passing ownership of data; maybe copy PQunescapeBytea code?
    PQfreemem(unescapedData);
    return result;
}

//==================================================================================
//Return the value for a given column for the current record - Private const version
QVariant PostgresqlCursor::pValue(int pos) const
{
//  KDbDrvWarn << "PostgresqlCursor::value - ERROR: requested position is greater than the number of fields";
    const qint64 row = at();

    KDbField *f = (m_fieldsExpanded && pos < qMin(m_fieldsExpanded->count(), m_fieldCount))
                       ? m_fieldsExpanded->at(pos)->field : 0;
// KDbDrvDbg << "pos:" << pos;

    const QVariant::Type type = m_realTypes[pos];
    const KDbField::Type kdbType = f ? f->type() : KDbField::InvalidType; // cache: evaluating type of expressions can be expensive
    const QVariant::Type kdbVariantType = KDbField::variantType(kdbType);
    if (PQgetisnull(d->res, row, pos) || kdbType == KDbField::Null) {
        return QVariant();
    }
    const char *data = PQgetvalue(d->res, row, pos);
    const int len = PQgetlength(d->res, row, pos);

    switch (type) { // from most to least frequently used types:
    case QVariant::String:
        return convertToKDbType(!KDbField::isTextType(kdbType),
                                d->unicode ? QString::fromUtf8(data, len) : QString::fromLatin1(data, len),
                                kdbVariantType);
    case QVariant::Int:
        return convertToKDbType(!KDbField::isIntegerType(kdbType),
                                atoi(data), // the fastest way
                                kdbVariantType);
    case QVariant::Bool:
        return convertToKDbType(kdbType != KDbField::Boolean,
                                bool(data[0] == 't'),
                                kdbVariantType);
    case QVariant::LongLong:
        return convertToKDbType(kdbType != KDbField::BigInteger,
                                (data[0] == '-') ? QByteArray::fromRawData(data, len).toLongLong()
                                                 : QByteArray::fromRawData(data, len).toULongLong(),
                                kdbVariantType);
    case QVariant::Double:
//! @todo support equivalent of QSql::NumericalPrecisionPolicy, especially for NUMERICOID
        return convertToKDbType(!KDbField::isFPNumericType(kdbType),
                                QByteArray::fromRawData(data, len).toDouble(),
                                kdbVariantType);
    case QVariant::Date:
        return convertToKDbType(kdbType != KDbField::Date,
                                (len == 0) ? QVariant(QDate())
                                           : QVariant(QDate::fromString(QLatin1String(QByteArray::fromRawData(data, len)), Qt::ISODate)),
                                kdbVariantType);
    case QVariant::Time:
        return convertToKDbType(kdbType != KDbField::Time,
                                timeFromData(data, len),
                                kdbVariantType);
    case QVariant::DateTime:
        return convertToKDbType(kdbType != KDbField::DateTime,
                                dateTimeFromData(data, len),
                                kdbVariantType);
    case QVariant::ByteArray:
        return convertToKDbType(kdbType != KDbField::BLOB,
                                byteArrayFromData(data),
                                kdbVariantType);
    default:
        qCWarning(KDB_LOG) << "PostgresqlCursor::pValue() data type?";
    }
    return value.value<kdbVariantType>();
}

//==================================================================================
//Return the current record as a char**
const char** PostgresqlCursor::recordData() const
{
    //! @todo
    return 0;
}

//==================================================================================
//Store the current record in [data]
bool PostgresqlCursor::drv_storeCurrentRecord(KDbRecordData* data) const
{
// KDbDrvDbg << "POSITION IS" << (long)m_at;
    for (int i = 0; i < m_fieldsToStoreInRecord; i++)
        (*data)[i] = pValue(i);
    return true;
}

//==================================================================================
//
/*void PostgresqlCursor::drv_clearServerResult()
{
//! @todo PostgresqlCursor: stuff with server results
}*/

//==================================================================================
//Add the current record to the internal buffer
//Implementation required but no need in this driver
//Result set is a buffer so do not need another
void PostgresqlCursor::drv_appendCurrentRecordToBuffer()
{

}

//==================================================================================
//Move internal pointer to internal buffer +1
//Implementation required but no need in this driver
void PostgresqlCursor::drv_bufferMovePointerNext()
{

}

//==================================================================================
//Move internal pointer to internal buffer -1
//Implementation required but no need in this driver
void PostgresqlCursor::drv_bufferMovePointerPrev()
{

}

//==================================================================================
//Move internal pointer to internal buffer to N
//Implementation required but no need in this driver
void PostgresqlCursor::drv_bufferMovePointerTo(qint64 to)
{
    Q_UNUSED(to);
}
