/* This file is part of the KDE project
   Copyright (C) 2004-2018 Jarosław Staniek <staniek@kde.org>
   Copyright (c) 2006, 2007 Thomas Braxton <kde.braxton@gmail.com>
   Copyright (c) 1999 Preston Brown <pbrown@kde.org>
   Copyright (c) 1997 Matthias Kalle Dalheimer <kalle@kde.org>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public License
   along with this library; see the file COPYING.LIB.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
*/

#include "KDb.h"
#include "KDbConnection.h"
#include "KDbConnectionData.h"
#include "KDbCursor.h"
#include "KDbDateTime.h"
#include "KDbDriverBehavior.h"
#include "KDbDriverManager.h"
#include "KDbDriver_p.h"
#include "KDbLookupFieldSchema.h"
#include "KDbMessageHandler.h"
#include "KDbNativeStatementBuilder.h"
#include "KDbQuerySchema.h"
#include "KDbRecordData.h"
#include "KDbSqlResult.h"
#include "KDbTableOrQuerySchema.h"
#include "KDbVersionInfo.h"
#include "KDb_p.h"
#include "kdb_debug.h"
#include "transliteration/transliteration_table.h"

#include <QMap>
#include <QHash>
#include <QBuffer>
#include <QPixmap>
#include <QSet>
#include <QTimer>
#include <QThread>
#include <QProgressDialog>
#include <QDomNode>
#include <QApplication>
#include <QDir>
#include <QProcess>
#include <QtDebug>

#include <limits>
#include <memory>

Q_DECLARE_METATYPE(KDbField::Type)

class ConnectionTestDialog;

class ConnectionTestThread : public QThread
{
    Q_OBJECT
public:
    ConnectionTestThread(ConnectionTestDialog *dlg, const KDbConnectionData& connData);
    void run() override;
Q_SIGNALS:
    void error(const QString& msg, const QString& details);
protected:
    void emitError(const KDbResultable& KDbResultable);

    ConnectionTestDialog* m_dlg;
    KDbConnectionData m_connData;
    KDbDriver *m_driver;
private:
    Q_DISABLE_COPY(ConnectionTestThread)
};

class ConnectionTestDialog : public QProgressDialog // krazy:exclude=qclasses
{
    Q_OBJECT
public:
    ConnectionTestDialog(const KDbConnectionData& data, KDbMessageHandler* msgHandler,
                         QWidget* parent = nullptr);
    ~ConnectionTestDialog() override;

    int exec() override;

public Q_SLOTS:
    void error(const QString& msg, const QString& details);

protected Q_SLOTS:
    void slotTimeout();
    void accept() override;
    void reject() override;

protected:
    void finish();

    QPointer<ConnectionTestThread> m_thread;
    KDbConnectionData m_connData;
    QTimer m_timer;
    KDbMessageHandler* m_msgHandler;
    int m_elapsedTime;
    bool m_error;
    QString m_msg;
    QString m_details;
    bool m_stopWaiting;

private:
    Q_DISABLE_COPY(ConnectionTestDialog)
};

ConnectionTestThread::ConnectionTestThread(ConnectionTestDialog* dlg, const KDbConnectionData& connData)
        : m_dlg(dlg), m_connData(connData)
{
    connect(this, SIGNAL(error(QString,QString)),
            dlg, SLOT(error(QString,QString)), Qt::QueuedConnection);

    // try to load driver now because it's not supported in different thread
    KDbDriverManager manager;
    m_driver = manager.driver(m_connData.driverId());
    if (manager.result().isError()) {
        emitError(*manager.resultable());
        m_driver = nullptr;
    }
}

void ConnectionTestThread::emitError(const KDbResultable& KDbResultable)
{
    QString msg;
    QString details;
    KDb::getHTMLErrorMesage(KDbResultable, &msg, &details);
    emit error(msg, details);
}

void ConnectionTestThread::run()
{
    if (!m_driver) {
        return;
    }
    QScopedPointer<KDbConnection> conn(m_driver->createConnection(m_connData));
    if (conn.isNull() || m_driver->result().isError()) {
        emitError(*m_driver);
        return;
    }
    if (!conn->connect() || conn->result().isError()) {
        emitError(*conn);
        return;
    }
    // SQL database backends like PostgreSQL require executing "USE database"
    // if we really want to know connection to the server succeeded.
    QString tmpDbName;
    if (!conn->useTemporaryDatabaseIfNeeded(&tmpDbName)) {
        emitError(*conn);
        return;
    }
    if (!tmpDbName.isEmpty()) {
        if (!conn->closeDatabase()) {
            emitError(*conn);
        }
    }
    emitError(KDbResultable());
}

ConnectionTestDialog::ConnectionTestDialog(const KDbConnectionData& data,
        KDbMessageHandler* msgHandler, QWidget* parent)
        : QProgressDialog(parent)
        , m_thread(new ConnectionTestThread(this, data))
        , m_connData(data)
        , m_msgHandler(msgHandler)
        , m_elapsedTime(0)
        , m_error(false)
        , m_stopWaiting(false)
{
    setWindowTitle(tr("Test Connection", "Dialog's title: testing database connection"));
    setLabelText(tr("Testing connection to \"%1\" database server...")
                 .arg(data.toUserVisibleString()));
    setModal(true);
    setRange(0, 0); //to show busy indicator
    connect(&m_timer, SIGNAL(timeout()), this, SLOT(slotTimeout()));
    adjustSize();
    resize(250, height());
}

ConnectionTestDialog::~ConnectionTestDialog()
{
    if (m_thread->isRunning()) {
        m_thread->terminate();
    }
    m_thread->deleteLater();
}

int ConnectionTestDialog::exec()
{
    //kdbDebug() << "tid:" << QThread::currentThread() << "this_thread:" << thread();
    m_timer.start(20);
    m_thread->start();
    const int res = QProgressDialog::exec(); // krazy:exclude=qclasses
    m_thread->wait();
    m_timer.stop();
    return res;
}

void ConnectionTestDialog::slotTimeout()
{
    //kdbDebug() << "tid:" << QThread::currentThread() << "this_thread:" << thread();
    //kdbDebug() << m_error;
    bool notResponding = false;
    if (m_elapsedTime >= 1000*5) {//5 seconds
        m_stopWaiting = true;
        notResponding = true;
    }
    //kdbDebug() << m_elapsedTime << m_stopWaiting << notResponding;
    if (m_stopWaiting) {
        m_timer.disconnect(this);
        m_timer.stop();
        QString message;
        QString details;
        KDbMessageHandler::MessageType type;
        if (m_error) {
            reject();
            //kdbDebug() << "after reject";
            message = tr("Test connection to \"%1\" database server failed.")
                         .arg(m_connData.toUserVisibleString());
            details = m_msg;
            if (!m_details.isEmpty()) {
                details += QLatin1Char('\n') + m_details;
            }
            type = KDbMessageHandler::Sorry;
            m_error = false;
        } else if (notResponding) {
            reject();
            //kdbDebug() << "after reject";
            message = tr("Test connection to \"%1\" database server failed. The server is not responding.")
                         .arg(m_connData.toUserVisibleString());
            type = KDbMessageHandler::Sorry;
        } else {
            accept();
            //kdbDebug() << "after accept";
            message = tr("Test connection to \"%1\" database server established successfully.")
                         .arg(m_connData.toUserVisibleString()),
            type = KDbMessageHandler::Information;
        }
        if (m_msgHandler) {
            m_msgHandler->showErrorMessage(type, message, details, tr("Test Connection"));
        }
        return;
    }
    m_elapsedTime += 20;
    setValue(m_elapsedTime);
}

void ConnectionTestDialog::error(const QString& msg, const QString& details)
{
    //kdbDebug() << "tid:" << QThread::currentThread() << "this_thread:" << thread();
    //kdbDebug() << msg << details;
    m_stopWaiting = true;
    m_msg = msg;
    m_details = details;
    m_error = !msg.isEmpty() || !details.isEmpty();
    if (m_error) {
        kdbDebug() << "Error:" << msg << details;
    }
}

void ConnectionTestDialog::accept()
{
    finish();
    QProgressDialog::accept(); // krazy:exclude=qclasses
}

void ConnectionTestDialog::reject()
{
    finish();
    QProgressDialog::reject(); // krazy:exclude=qclasses
}

void ConnectionTestDialog::finish()
{
    if (m_thread->isRunning()) {
        m_thread->terminate();
    }
    m_timer.disconnect(this);
    m_timer.stop();
}

// ----

//! @return hex digit converted to integer (0 to 15), 0xFF on failure
inline static unsigned char hexDigitToInt(char digit)
{
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f') {
        return digit - 'a' + 10;
    }
    if (digit >= 'A' && digit <= 'F') {
        return digit - 'A' + 10;
    }
    return 0xFF;
}

//! Converts textual representation @a data of a hex number (@a length digits) to a byte array @a array
//! @return true on success and false if @a data contains characters that are not hex digits.
//! true is returned for empty @a data as well.
inline static bool hexToByteArrayInternal(const char* data, int length, QByteArray *array)
{
    Q_ASSERT(length >= 0);
    Q_ASSERT(data || length == 0);
    array->resize(length / 2 + length % 2);
    for(int i = 0; length > 0; --length, ++data, ++i) {
        unsigned char d1 = hexDigitToInt(data[0]);
        unsigned char d2;
        if (i == 0 && (length % 2) == 1) { // odd number of digits; no leading 0
            d2 = d1;
            d1 = 0;
        }
        else {
            --length;
            ++data;
            d2 = hexDigitToInt(data[0]);
        }
        if (d1 == 0xFF || d2 == 0xFF) {
            return false;
        }
        (*array)[i] = (d1 << 4) + d2;
    }
    return true;
}

KDbVersionInfo KDb::version()
{
    return KDbVersionInfo(
        KDB_VERSION_MAJOR, KDB_VERSION_MINOR, KDB_VERSION_PATCH);
}

bool KDb::deleteRecords(KDbConnection* conn, const QString &tableName,
                        const QString &keyname, KDbField::Type keytype, const QVariant &keyval)
{
    return conn
        ? conn->executeSql(KDbEscapedString("DELETE FROM %1 WHERE %2=%3")
                               .arg(conn->escapeIdentifier(tableName))
                               .arg(conn->escapeIdentifier(keyname))
                               .arg(conn->driver()->valueToSql(keytype, keyval)))
        : false;
}

bool KDb::deleteRecords(KDbConnection* conn, const QString &tableName,
                        const QString &keyname1, KDbField::Type keytype1, const QVariant& keyval1,
                        const QString &keyname2, KDbField::Type keytype2, const QVariant& keyval2)
{
    return conn
        ? conn->executeSql(KDbEscapedString("DELETE FROM %1 WHERE %2=%3 AND %4=%5")
                               .arg(conn->escapeIdentifier(tableName))
                               .arg(conn->escapeIdentifier(keyname1))
                               .arg(conn->driver()->valueToSql(keytype1, keyval1))
                               .arg(conn->escapeIdentifier(keyname2))
                               .arg(conn->driver()->valueToSql(keytype2, keyval2)))
        : false;
}

bool KDb::deleteRecords(KDbConnection* conn, const QString &tableName,
                        const QString &keyname1, KDbField::Type keytype1, const QVariant& keyval1,
                        const QString &keyname2, KDbField::Type keytype2, const QVariant& keyval2,
                        const QString &keyname3, KDbField::Type keytype3, const QVariant& keyval3)
{
    return conn
        ? conn->executeSql(KDbEscapedString("DELETE FROM %1 WHERE %2=%3 AND %4=%5 AND %6=%7")
                               .arg(conn->escapeIdentifier(tableName))
                               .arg(conn->escapeIdentifier(keyname1))
                               .arg(conn->driver()->valueToSql(keytype1, keyval1))
                               .arg(conn->escapeIdentifier(keyname2))
                               .arg(conn->driver()->valueToSql(keytype2, keyval2))
                               .arg(conn->escapeIdentifier(keyname3))
                               .arg(conn->driver()->valueToSql(keytype3, keyval3)))
        : false;
}

bool KDb::deleteAllRecords(KDbConnection* conn, const QString &tableName)
{
    return conn
        ? conn->executeSql(
              KDbEscapedString("DELETE FROM %1").arg(conn->escapeIdentifier(tableName)))
        : false;
}

KDB_EXPORT quint64 KDb::lastInsertedAutoIncValue(QSharedPointer<KDbSqlResult> result,
                                                 const QString &autoIncrementFieldName,
                                                 const QString &tableName, quint64 *recordId)
{
    if (!result) {
        return std::numeric_limits<quint64>::max();
    }
    const quint64 foundRecordId = result->lastInsertRecordId();
    if (recordId) {
        *recordId = foundRecordId;
    }
    return KDb::lastInsertedAutoIncValue(result->connection(),
                                         foundRecordId, autoIncrementFieldName, tableName);
}

KDB_EXPORT quint64 KDb::lastInsertedAutoIncValue(KDbConnection *conn, const quint64 recordId,
                                                 const QString &autoIncrementFieldName,
                                                 const QString &tableName)
{
    const KDbDriverBehavior *behavior = KDbDriverPrivate::behavior(conn->driver());
    if (behavior->ROW_ID_FIELD_RETURNS_LAST_AUTOINCREMENTED_VALUE) {
        return recordId;
    }
    KDbRecordData rdata;
    if (recordId == std::numeric_limits<quint64>::max()
        || true != conn->querySingleRecord(
                  KDbEscapedString("SELECT ") + escapeIdentifier(tableName) + '.'
                + escapeIdentifier(autoIncrementFieldName)
                + " FROM " + escapeIdentifier(tableName)
                + " WHERE " + behavior->ROW_ID_FIELD_NAME
                + '=' + KDbEscapedString::number(recordId), &rdata))
    {
        return std::numeric_limits<quint64>::max();
    }
    return rdata[0].toULongLong();
}

bool KDb::isEmptyValue(KDbField::Type type, const QVariant &value)
{
    if (KDbField::isTextType(type)) {
        return value.toString().isEmpty() && !value.toString().isNull();
    }
    else if (type == KDbField::BLOB) {
        return value.toByteArray().isEmpty() && !value.toByteArray().isNull();
    }
    return value.isNull();
}

KDbEscapedString KDb::sqlWhere(KDbDriver *drv, KDbField::Type t,
                            const QString& fieldName, const QVariant& value)
{
    if (value.isNull())
        return KDbEscapedString(fieldName) + " IS NULL";
    return KDbEscapedString(fieldName) + '=' + drv->valueToSql(t, value);
}

//! Cache
struct TypeCache {
    TypeCache() {
        for (KDbField::Type t = KDbField::InvalidType; t <= KDbField::LastType; t = KDbField::Type(int(t) + 1)) {
            const KDbField::TypeGroup tg = KDbField::typeGroup(t);
            QList<KDbField::Type> list;
            QStringList name_list, str_list;
            if (tlist.contains(tg)) {
                list = tlist.value(tg);
                name_list = nlist.value(tg);
                str_list = slist.value(tg);
            }
            list += t;
            name_list += KDbField::typeName(t);
            str_list += KDbField::typeString(t);
            tlist[ tg ] = list;
            nlist[ tg ] = name_list;
            slist[ tg ] = str_list;
        }

        def_tlist[ KDbField::InvalidGroup ] = KDbField::InvalidType;
        def_tlist[ KDbField::TextGroup ] = KDbField::Text;
        def_tlist[ KDbField::IntegerGroup ] = KDbField::Integer;
        def_tlist[ KDbField::FloatGroup ] = KDbField::Double;
        def_tlist[ KDbField::BooleanGroup ] = KDbField::Boolean;
        def_tlist[ KDbField::DateTimeGroup ] = KDbField::Date;
        def_tlist[ KDbField::BLOBGroup ] = KDbField::BLOB;
    }

    QHash< KDbField::TypeGroup, QList<KDbField::Type> > tlist;
    QHash< KDbField::TypeGroup, QStringList > nlist;
    QHash< KDbField::TypeGroup, QStringList > slist;
    QHash< KDbField::TypeGroup, KDbField::Type > def_tlist;
};

Q_GLOBAL_STATIC(TypeCache, KDb_typeCache)

const QList<KDbField::Type> KDb::fieldTypesForGroup(KDbField::TypeGroup typeGroup)
{
    return KDb_typeCache->tlist.value(typeGroup);
}

QStringList KDb::fieldTypeNamesForGroup(KDbField::TypeGroup typeGroup)
{
    return KDb_typeCache->nlist.value(typeGroup);
}

QStringList KDb::fieldTypeStringsForGroup(KDbField::TypeGroup typeGroup)
{
    return KDb_typeCache->slist.value(typeGroup);
}

KDbField::Type KDb::defaultFieldTypeForGroup(KDbField::TypeGroup typeGroup)
{
    return (typeGroup <= KDbField::LastTypeGroup) ? KDb_typeCache->def_tlist.value(typeGroup) : KDbField::InvalidType;
}

void KDb::getHTMLErrorMesage(const KDbResultable& resultable, QString *msg, QString *details)
{
    if (!msg) {
        kdbWarning() << "Missing 'msg' parameter";
        return;
    }
    if (!details) {
        kdbWarning() << "Missing 'details' parameter";
        return;
    }
    const KDbResult result(resultable.result());
    if (!result.isError())
        return;
    //lower level message is added to the details, if there is alread message specified
    if (!result.messageTitle().isEmpty())
        *msg += QLatin1String("<p>") + result.messageTitle();

    if (msg->isEmpty())
        *msg = QLatin1String("<p>") + result.message();
    else
        *details += QLatin1String("<p>") + result.message();

    if (!result.serverMessage().isEmpty())
        *details += QLatin1String("<p><b>") + kdb::tr("Message from server:")
                   + QLatin1String("</b> ") + result.serverMessage();
    if (!result.recentSqlString().isEmpty())
        *details += QLatin1String("<p><b>") + kdb::tr("SQL statement:")
                   + QString::fromLatin1("</b> <tt>%1</tt>").arg(result.recentSqlString().toString());
    int serverErrorCode = 0;
    QString serverResultName;
    if (result.isError()) {
        serverErrorCode = result.serverErrorCode();
        serverResultName = resultable.serverResultName();
    }
    if (   !details->isEmpty()
        && (   !result.serverMessage().isEmpty()
            || !result.recentSqlString().isEmpty()
            || !serverResultName.isEmpty()
            || serverErrorCode != 0)
           )
    {
        *details += (QLatin1String("<p><b>") + kdb::tr("Server result code:")
                    + QLatin1String("</b> ") + QString::number(serverErrorCode));
        if (!serverResultName.isEmpty()) {
            *details += QString::fromLatin1(" (%1)").arg(serverResultName);
        }
    }
    else {
        if (!serverResultName.isEmpty()) {
            *details += (QLatin1String("<p><b>") + kdb::tr("Server result:")
                        + QLatin1String("</b> ") + serverResultName);
        }
    }

    if (!details->isEmpty() && !details->startsWith(QLatin1String("<qt>"))) {
        if (!details->startsWith(QLatin1String("<p>")))
            details->prepend(QLatin1String("<p>"));
    }
}

void KDb::getHTMLErrorMesage(const KDbResultable& resultable, QString *msg)
{
    getHTMLErrorMesage(resultable, msg, msg);
}

void KDb::getHTMLErrorMesage(const KDbResultable& resultable, KDbResultInfo *info)
{
    if (!info) {
        kdbWarning() << "Missing 'info' parameter";
        return;
    }
    getHTMLErrorMesage(resultable, &info->message, &info->description);
}

tristate KDb::idForObjectName(KDbConnection* conn, int *id, const QString& objName, int objType)
{
    return conn
        ? conn->querySingleNumber(
              KDbEscapedString("SELECT o_id FROM kexi__objects WHERE o_name=%1 AND o_type=%2")
                  .arg(conn->escapeString(objName))
                  .arg(objType),
              id)
        : false;
}

//-----------------------------------------

tristate KDb::showConnectionTestDialog(QWidget *parent, const KDbConnectionData &data,
                                   KDbMessageHandler *msgHandler)
{
    ConnectionTestDialog dlg(data, msgHandler, parent);
    const int result = dlg.exec();
    if (dlg.wasCanceled()) {
        return cancelled;
    }
    return result == QDialog::Accepted;
}

bool KDb::splitToTableAndFieldParts(const QString& string,
                                          QString *tableName, QString *fieldName,
                                          SplitToTableAndFieldPartsOptions option)
{
    if (!tableName || !fieldName) {
        return false;
    }
    const int id = string.indexOf(QLatin1Char('.'));
    if (option & SetFieldNameIfNoTableName && id == -1) {
        tableName->clear();
        *fieldName = string;
        return !fieldName->isEmpty();
    }
    if (id <= 0 || id == int(string.length() - 1))
        return false;
    *tableName = string.left(id);
    *fieldName = string.mid(id + 1);
    return !tableName->isEmpty() && !fieldName->isEmpty();
}

bool KDb::supportsVisibleDecimalPlacesProperty(KDbField::Type type)
{
//! @todo add check for decimal type as well
    return KDbField::isFPNumericType(type);
}

inline static QString numberToString(double value, int decimalPlaces, const QLocale *locale)
{
//! @todo round?
    QString result;
    if (decimalPlaces == 0) {
        result = locale ? locale->toString(qlonglong(value))
                        : QString::number(qlonglong(value));
    } else {
        const int realDecimalPlaces = decimalPlaces < 0 ? 10 : decimalPlaces;
        result = locale ? locale->toString(value, 'f', realDecimalPlaces)
                        : QString::number(value, 'f', realDecimalPlaces);
        if (decimalPlaces < 0) { // cut off zeros
            int i = result.length() - 1;
            while (i > 0 && result[i] == QLatin1Char('0')) {
                i--;
            }
            if (result[i].isDigit()) {// last digit
                ++i;
            }
            result.truncate(i);
        }
    }
    return result;
}

QString KDb::numberToString(double value, int decimalPlaces)
{
    return ::numberToString(value, decimalPlaces, nullptr);
}

QString KDb::numberToLocaleString(double value, int decimalPlaces)
{
    QLocale defaultLocale;
    return ::numberToString(value, decimalPlaces, &defaultLocale);
}

QString KDb::numberToLocaleString(double value, int decimalPlaces, const QLocale &locale)
{
    return ::numberToString(value, decimalPlaces, &locale);
}

KDbField::Type KDb::intToFieldType(int type)
{
    if (type < int(KDbField::InvalidType) || type > int(KDbField::LastType)) {
        return KDbField::InvalidType;
    }
    return static_cast<KDbField::Type>(type);
}

KDbField::TypeGroup KDb::intToFieldTypeGroup(int typeGroup)
{
    if (typeGroup < int(KDbField::InvalidGroup) || typeGroup > int(KDbField::LastTypeGroup)) {
        return KDbField::InvalidGroup;
    }
    return static_cast<KDbField::TypeGroup>(typeGroup);
}

static bool setIntToFieldType(KDbField *field, const QVariant& value)
{
    Q_ASSERT(field);
    bool ok;
    const int intType = value.toInt(&ok);
    if (!ok) {//for sanity
        kdbWarning() << "Could not convert value" << value << "to field type";
        return false;
    }
    if (KDbField::InvalidType == KDb::intToFieldType(intType)) {//for sanity
        kdbWarning() << "Invalid field type" << intType;
        return false;
    }
    field->setType((KDbField::Type)intType);
    return true;
}

//! @internal for KDb::isBuiltinTableFieldProperty()
struct KDb_BuiltinFieldProperties {
    KDb_BuiltinFieldProperties() {
#define ADD(name) set.insert(name)
        ADD("type");
        ADD("primaryKey");
        ADD("indexed");
        ADD("autoIncrement");
        ADD("unique");
        ADD("notNull");
        ADD("allowEmpty");
        ADD("unsigned");
        ADD("name");
        ADD("caption");
        ADD("description");
        ADD("maxLength");
        ADD("maxLengthIsDefault");
        ADD("precision");
        ADD("defaultValue");
        ADD("defaultWidth");
        ADD("visibleDecimalPlaces");
//! @todo always update this when new builtins appear!
#undef ADD
    }
    QSet<QByteArray> set;
};

//! for KDb::isBuiltinTableFieldProperty()
Q_GLOBAL_STATIC(KDb_BuiltinFieldProperties, KDb_builtinFieldProperties)


bool KDb::isBuiltinTableFieldProperty(const QByteArray& propertyName)
{
    return KDb_builtinFieldProperties->set.contains(propertyName);
}

static QVariant visibleColumnValue(const KDbLookupFieldSchema *lookup)
{
    if (!lookup || lookup->visibleColumns().count() == 1) {
        if (lookup) {
            const QList<int> visibleColumns = lookup->visibleColumns();
            if (!visibleColumns.isEmpty()) {
                return visibleColumns.first();
            }
        }
        return QVariant();
    }
    QList<QVariant> variantList;
    const QList<int> visibleColumns(lookup->visibleColumns());
    for(int column : visibleColumns) {
        variantList.append(column);
    }
    return variantList;
}

void KDb::getProperties(const KDbLookupFieldSchema *lookup, QMap<QByteArray, QVariant> *values)
{
    if (!values) {
        return;
    }
    KDbLookupFieldSchemaRecordSource recordSource;
    if (lookup) {
        recordSource = lookup->recordSource();
    }
    values->insert("rowSource", lookup ? recordSource.name() : QVariant());
    values->insert("rowSourceType", lookup ? recordSource.typeName() : QVariant());
    values->insert("rowSourceValues",
        (lookup && !recordSource.values().isEmpty()) ? recordSource.values() : QVariant());
    values->insert("boundColumn", lookup ? lookup->boundColumn() : QVariant());
    values->insert("visibleColumn", visibleColumnValue(lookup));
   QList<QVariant> variantList;
    if (lookup) {
        const QList<int> columnWidths = lookup->columnWidths();
        for(const QVariant& variant : columnWidths) {
            variantList.append(variant);
        }
    }
    values->insert("columnWidths", lookup ? variantList : QVariant());
    values->insert("showColumnHeaders", lookup ? lookup->columnHeadersVisible() : QVariant());
    values->insert("listRows", lookup ? lookup->maxVisibleRecords() : QVariant());
    values->insert("limitToList", lookup ? lookup->limitToList() : QVariant());
    values->insert("displayWidget", lookup ? int(lookup->displayWidget()) : QVariant());
}

void KDb::getFieldProperties(const KDbField &field, QMap<QByteArray, QVariant> *values)
{
    if (!values) {
        return;
    }
    values->clear();
    // normal values
    values->insert("type", field.type());
    const KDbField::Constraints constraints = field.constraints();
    values->insert("primaryKey", constraints.testFlag(KDbField::PrimaryKey));
    values->insert("indexed", constraints.testFlag(KDbField::Indexed));
    values->insert("autoIncrement", KDbField::isAutoIncrementAllowed(field.type())
                                    && constraints.testFlag(KDbField::AutoInc));
    values->insert("unique", constraints.testFlag(KDbField::Unique));
    values->insert("notNull", constraints.testFlag(KDbField::NotNull));
    values->insert("allowEmpty", !constraints.testFlag(KDbField::NotEmpty));
    const KDbField::Options options = field.options();
    values->insert("unsigned", options.testFlag(KDbField::Unsigned));
    values->insert("name", field.name());
    values->insert("caption", field.caption());
    values->insert("description", field.description());
    values->insert("maxLength", field.maxLength());
    values->insert("maxLengthIsDefault", field.maxLengthStrategy() & KDbField::DefaultMaxLength);
    values->insert("precision", field.precision());
    values->insert("defaultValue", field.defaultValue());
//! @todo IMPORTANT: values->insert("defaultWidth", field.defaultWidth());
    if (KDb::supportsVisibleDecimalPlacesProperty(field.type())) {
        values->insert("visibleDecimalPlaces", field.defaultValue());
    }
    // insert lookup-related values
    const KDbLookupFieldSchema *lookup = field.table()->lookupFieldSchema(field);
    KDb::getProperties(lookup, values);
}

static bool containsLookupFieldSchemaProperties(const QMap<QByteArray, QVariant>& values)
{
    for (QMap<QByteArray, QVariant>::ConstIterator it(values.constBegin());
         it != values.constEnd(); ++it)
    {
        if (KDb::isLookupFieldSchemaProperty(it.key())) {
            return true;
        }
    }
    return false;
}

bool KDb::setFieldProperties(KDbField *field, const QMap<QByteArray, QVariant>& values)
{
    if (!field) {
        return false;
    }
    QMap<QByteArray, QVariant>::ConstIterator it;
    if ((it = values.find("type")) != values.constEnd()) {
        if (!setIntToFieldType(field, *it))
            return false;
    }

#define SET_BOOLEAN_FLAG(flag, value) { \
        constraints |= KDbField::flag; \
        if (!value) \
            constraints ^= KDbField::flag; \
    }

    KDbField::Constraints constraints = field->constraints();
    bool ok = true;
    if ((it = values.find("primaryKey")) != values.constEnd())
        SET_BOOLEAN_FLAG(PrimaryKey, (*it).toBool());
    if ((it = values.find("indexed")) != values.constEnd())
        SET_BOOLEAN_FLAG(Indexed, (*it).toBool());
    if ((it = values.find("autoIncrement")) != values.constEnd()
            && KDbField::isAutoIncrementAllowed(field->type()))
        SET_BOOLEAN_FLAG(AutoInc, (*it).toBool());
    if ((it = values.find("unique")) != values.constEnd())
        SET_BOOLEAN_FLAG(Unique, (*it).toBool());
    if ((it = values.find("notNull")) != values.constEnd())
        SET_BOOLEAN_FLAG(NotNull, (*it).toBool());
    if ((it = values.find("allowEmpty")) != values.constEnd())
        SET_BOOLEAN_FLAG(NotEmpty, !(*it).toBool());
    field->setConstraints(constraints);

    KDbField::Options options;
    if ((it = values.find("unsigned")) != values.constEnd()) {
        options |= KDbField::Unsigned;
        if (!(*it).toBool())
            options ^= KDbField::Unsigned;
    }
    field->setOptions(options);

    if ((it = values.find("name")) != values.constEnd())
        field->setName((*it).toString());
    if ((it = values.find("caption")) != values.constEnd())
        field->setCaption((*it).toString());
    if ((it = values.find("description")) != values.constEnd())
        field->setDescription((*it).toString());
    if ((it = values.find("maxLength")) != values.constEnd())
        field->setMaxLength((*it).isNull() ? 0/*default*/ : (*it).toInt(&ok));
    if (!ok)
        return false;
    if ((it = values.find("maxLengthIsDefault")) != values.constEnd()
            && (*it).toBool())
    {
        field->setMaxLengthStrategy(KDbField::DefaultMaxLength);
    }
    if ((it = values.find("precision")) != values.constEnd())
        field->setPrecision((*it).isNull() ? 0/*default*/ : (*it).toInt(&ok));
    if (!ok)
        return false;
    if ((it = values.find("defaultValue")) != values.constEnd())
        field->setDefaultValue(*it);
//! @todo IMPORTANT: defaultWidth
#if 0
    if ((it = values.find("defaultWidth")) != values.constEnd())
        field.setDefaultWidth((*it).isNull() ? 0/*default*/ : (*it).toInt(&ok));
    if (!ok)
        return false;
#endif

    // -- extended properties
    if ((it = values.find("visibleDecimalPlaces")) != values.constEnd()
            && KDb::supportsVisibleDecimalPlacesProperty(field->type()))
        field->setVisibleDecimalPlaces((*it).isNull() ? -1/*default*/ : (*it).toInt(&ok));
    if (!ok)
        return false;

    if (field->table() && containsLookupFieldSchemaProperties(values)) {
        KDbLookupFieldSchema *lookup = field->table()->lookupFieldSchema(*field);
        QScopedPointer<KDbLookupFieldSchema> createdLookup;
        if (!lookup) { // create lookup if needed
            createdLookup.reset(lookup = new KDbLookupFieldSchema());
        }
        if (lookup->setProperties(values)) {
            if (createdLookup) {
                if (field->table()->setLookupFieldSchema(field->name(), lookup)) {
                    createdLookup.take(); // ownership passed
                    lookup = nullptr;
                }
            }
        }
    }

    return true;
#undef SET_BOOLEAN_FLAG
}

//! @internal for isExtendedTableProperty()
struct KDb_ExtendedProperties {
    KDb_ExtendedProperties() {
#define ADD(name) set.insert( name )
        ADD("visibledecimalplaces");
        ADD("rowsource");
        ADD("rowsourcetype");
        ADD("rowsourcevalues");
        ADD("boundcolumn");
        ADD("visiblecolumn");
        ADD("columnwidths");
        ADD("showcolumnheaders");
        ADD("listrows");
        ADD("limittolist");
        ADD("displaywidget");
#undef ADD
    }
    QSet<QByteArray> set;
};

//! for isExtendedTableProperty()
Q_GLOBAL_STATIC(KDb_ExtendedProperties, KDb_extendedProperties)

bool KDb::isExtendedTableFieldProperty(const QByteArray& propertyName)
{
    return KDb_extendedProperties->set.contains(QByteArray(propertyName).toLower());
}

//! @internal for isLookupFieldSchemaProperty()
struct KDb_LookupFieldSchemaProperties {
    KDb_LookupFieldSchemaProperties() {
        QMap<QByteArray, QVariant> tmp;
        KDb::getProperties(nullptr, &tmp);
        for (QMap<QByteArray, QVariant>::ConstIterator it(tmp.constBegin());
             it != tmp.constEnd(); ++it)
        {
            set.insert(it.key().toLower());
        }
    }
    QSet<QByteArray> set;
};

//! for isLookupFieldSchemaProperty()
Q_GLOBAL_STATIC(KDb_LookupFieldSchemaProperties, KDb_lookupFieldSchemaProperties)

bool KDb::isLookupFieldSchemaProperty(const QByteArray& propertyName)
{
    return KDb_lookupFieldSchemaProperties->set.contains(propertyName.toLower());
}

bool KDb::setFieldProperty(KDbField *field, const QByteArray& propertyName, const QVariant& value)
{
    if (!field) {
        return false;
    }
#define SET_BOOLEAN_FLAG(flag, value) { \
        constraints |= KDbField::flag; \
        if (!value) \
            constraints ^= KDbField::flag; \
        field->setConstraints( constraints ); \
        return true; \
    }
#define GET_INT(method) { \
        const int ival = value.toInt(&ok); \
        if (!ok) \
            return false; \
        field->method( ival ); \
        return true; \
    }

    if (propertyName.isEmpty())
        return false;

    bool ok;
    if (KDb::isExtendedTableFieldProperty(propertyName)) {
        //a little speedup: identify extended property in O(1)
        if ("visibleDecimalPlaces" == propertyName
                && KDb::supportsVisibleDecimalPlacesProperty(field->type())) {
            GET_INT(setVisibleDecimalPlaces);
        }
        else if (KDb::isLookupFieldSchemaProperty(propertyName)) {
            if (!field->table()) {
                kdbWarning() << "Could not set" << propertyName << "property - no table assigned for field";
            } else {
                KDbLookupFieldSchema *lookup = field->table()->lookupFieldSchema(*field);
                const bool createLookup = !lookup;
                if (createLookup) // create lookup if needed
                    lookup = new KDbLookupFieldSchema();
                if (lookup->setProperty(propertyName, value)) {
                    if (createLookup)
                        field->table()->setLookupFieldSchema(field->name(), lookup);
                    return true;
                }
                if (createLookup)
                    delete lookup; // not set, delete
            }
        }
    } else {//non-extended
        if ("type" == propertyName)
            return setIntToFieldType(field, value);

        KDbField::Constraints constraints = field->constraints();
        if ("primaryKey" == propertyName)
            SET_BOOLEAN_FLAG(PrimaryKey, value.toBool());
        if ("indexed" == propertyName)
            SET_BOOLEAN_FLAG(Indexed, value.toBool());
        if ("autoIncrement" == propertyName
                && KDbField::isAutoIncrementAllowed(field->type()))
            SET_BOOLEAN_FLAG(AutoInc, value.toBool());
        if ("unique" == propertyName)
            SET_BOOLEAN_FLAG(Unique, value.toBool());
        if ("notNull" == propertyName)
            SET_BOOLEAN_FLAG(NotNull, value.toBool());
        if ("allowEmpty" == propertyName)
            SET_BOOLEAN_FLAG(NotEmpty, !value.toBool());

        KDbField::Options options;
        if ("unsigned" == propertyName) {
            options |= KDbField::Unsigned;
            if (!value.toBool())
                options ^= KDbField::Unsigned;
            field->setOptions(options);
            return true;
        }

        if ("name" == propertyName) {
            if (value.toString().isEmpty())
                return false;
            field->setName(value.toString());
            return true;
        }
        if ("caption" == propertyName) {
            field->setCaption(value.toString());
            return true;
        }
        if ("description" == propertyName) {
            field->setDescription(value.toString());
            return true;
        }
        if ("maxLength" == propertyName)
            GET_INT(setMaxLength);
        if ("maxLengthIsDefault" == propertyName) {
            field->setMaxLengthStrategy(KDbField::DefaultMaxLength);
        }
        if ("precision" == propertyName)
            GET_INT(setPrecision);
        if ("defaultValue" == propertyName) {
            field->setDefaultValue(value);
            return true;
        }

//! @todo IMPORTANT: defaultWidth
#if 0
        if ("defaultWidth" == propertyName)
            GET_INT(setDefaultWidth);
#endif
        // last chance that never fails: custom field property
        field->setCustomProperty(propertyName, value);
    }

    kdbWarning() << "Field property" << propertyName << "not found!";
    return false;
#undef SET_BOOLEAN_FLAG
#undef GET_INT
}

int KDb::loadIntPropertyValueFromDom(const QDomNode& node, bool* ok)
{
    QByteArray valueType = node.nodeName().toLatin1();
    if (valueType.isEmpty() || valueType != "number") {
        if (ok)
            *ok = false;
        return 0;
    }
    const QString text(QDomNode(node).toElement().text());
    int val = text.toInt(ok);
    return val;
}

QString KDb::loadStringPropertyValueFromDom(const QDomNode& node, bool* ok)
{
    QByteArray valueType = node.nodeName().toLatin1();
    if (valueType != "string") {
        if (ok)
            *ok = false;
        return QString();
    }
    if (ok)
        *ok = true;
    return QDomNode(node).toElement().text();
}

QVariant KDb::loadPropertyValueFromDom(const QDomNode& node, bool* ok)
{
    QByteArray valueType = node.nodeName().toLatin1();
    if (valueType.isEmpty()) {
        if (ok)
            *ok = false;
        return QVariant();
    }
    if (ok)
        *ok = true;
    const QString text(QDomNode(node).toElement().text());
    bool _ok;
    if (valueType == "string") {
        return text;
    }
    else if (valueType == "cstring") {
        return text.toLatin1();
    }
    else if (valueType == "number") { // integer or double
        if (text.indexOf(QLatin1Char('.')) != -1) {
            double val = text.toDouble(&_ok);
            if (_ok)
                return val;
        }
        else {
            const int val = text.toInt(&_ok);
            if (_ok)
                return val;
            const qint64 valLong = text.toLongLong(&_ok);
            if (_ok)
                return valLong;
        }
    }
    else if (valueType == "bool") {
        return text.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0
               || text == QLatin1String("1");
    }
    else {
//! @todo add more QVariant types
        kdbWarning() << "Unknown property type" << valueType;
    }
    if (ok)
        *ok = false;
    return QVariant();
}

QDomElement KDb::saveNumberElementToDom(QDomDocument *doc, QDomElement *parentEl,
        const QString& elementName, int value)
{
    if (!doc || !parentEl || elementName.isEmpty()) {
        return QDomElement();
    }
    QDomElement el(doc->createElement(elementName));
    parentEl->appendChild(el);
    QDomElement numberEl(doc->createElement(QLatin1String("number")));
    el.appendChild(numberEl);
    numberEl.appendChild(doc->createTextNode(QString::number(value)));
    return el;
}

QDomElement KDb::saveBooleanElementToDom(QDomDocument *doc, QDomElement *parentEl,
        const QString& elementName, bool value)
{
    if (!doc || !parentEl || elementName.isEmpty()) {
        return QDomElement();
    }
    QDomElement el(doc->createElement(elementName));
    parentEl->appendChild(el);
    QDomElement numberEl(doc->createElement(QLatin1String("bool")));
    el.appendChild(numberEl);
    numberEl.appendChild(doc->createTextNode(
                             value ? QLatin1String("true") : QLatin1String("false")));
    return el;
}

//! @internal Used in KDb::emptyValueForFieldType()
struct KDb_EmptyValueForFieldTypeCache {
    KDb_EmptyValueForFieldTypeCache()
            : values(int(KDbField::LastType) + 1) {
#define ADD(t, value) values.insert(t, value);
        ADD(KDbField::Byte, 0);
        ADD(KDbField::ShortInteger, 0);
        ADD(KDbField::Integer, 0);
        ADD(KDbField::BigInteger, 0);
        ADD(KDbField::Boolean, false);
        ADD(KDbField::Float, 0.0);
        ADD(KDbField::Double, 0.0);
//! @todo ok? we have no better defaults
        ADD(KDbField::Text, QLatin1String(" "));
        ADD(KDbField::LongText, QLatin1String(" "));
        ADD(KDbField::BLOB, QByteArray());
#undef ADD
    }
    QVector<QVariant> values;
};

//! Used in KDb::emptyValueForFieldType()
Q_GLOBAL_STATIC(KDb_EmptyValueForFieldTypeCache, KDb_emptyValueForFieldTypeCache)

QVariant KDb::emptyValueForFieldType(KDbField::Type type)
{
    const QVariant val(KDb_emptyValueForFieldTypeCache->values.at(
                           (type <= KDbField::LastType) ? type : KDbField::InvalidType));
    if (!val.isNull())
        return val;
    else { //special cases
        if (type == KDbField::Date)
            return QDate::currentDate();
        if (type == KDbField::DateTime)
            return QDateTime::currentDateTime();
        if (type == KDbField::Time)
            return QTime::currentTime();
    }
    kdbWarning() << "No empty value for field type" << KDbField::typeName(type);
    return QVariant();
}

//! @internal Used in KDb::notEmptyValueForFieldType()
struct KDb_NotEmptyValueForFieldTypeCache {
    KDb_NotEmptyValueForFieldTypeCache()
            : values(int(KDbField::LastType) + 1) {
#define ADD(t, value) values.insert(t, value);
        // copy most of the values
        for (int i = int(KDbField::InvalidType) + 1; i <= KDbField::LastType; i++) {
            if (i == KDbField::Date || i == KDbField::DateTime || i == KDbField::Time)
                continue; //'current' value will be returned
            if (i == KDbField::Text || i == KDbField::LongText) {
                ADD(i, QVariant(QLatin1String("")));
                continue;
            }
            if (i == KDbField::BLOB) {
//! @todo blobs will contain other MIME types too
                QByteArray ba;
//! @todo port to Qt4
#if 0
                QBuffer buffer(&ba);
                buffer.open(QIODevice::WriteOnly);
                QPixmap pm(SmallIcon("document-new"));
                pm.save(&buffer, "PNG"/*! @todo default? */);
#endif
                ADD(i, ba);
                continue;
            }
            ADD(i, KDb::emptyValueForFieldType((KDbField::Type)i));
        }
#undef ADD
    }
    QVector<QVariant> values;
};
//! Used in KDb::notEmptyValueForFieldType()
Q_GLOBAL_STATIC(KDb_NotEmptyValueForFieldTypeCache, KDb_notEmptyValueForFieldTypeCache)

QVariant KDb::notEmptyValueForFieldType(KDbField::Type type)
{
    const QVariant val(KDb_notEmptyValueForFieldTypeCache->values.at(
                           (type <= KDbField::LastType) ? type : KDbField::InvalidType));
    if (!val.isNull())
        return val;
    else { //special cases
        if (type == KDbField::Date)
            return QDate::currentDate();
        if (type == KDbField::DateTime)
            return QDateTime::currentDateTime();
        if (type == KDbField::Time)
            return QTime::currentTime();
    }
    kdbWarning() << "No non-empty value for field type" << KDbField::typeName(type);
    return QVariant();
}

//! @internal @return nestimated new length after escaping of string @a string
template<typename T>
inline static int estimatedNewLength(const T &string, bool addQuotes)
{
    if (string.length() < 10)
        return string.length() * 2 + (addQuotes ? 2 : 0);
    return string.length() * 3 / 2;
}

//! @internal @return @a string string with applied KDbSQL identifier escaping.
//! If @a addQuotes is true, '"' characer is prepended and appended.
template<typename T, typename Latin1StringType, typename Latin1CharType, typename CharType>
inline static T escapeIdentifier(const T& string, bool addQuotes)
{
    const Latin1CharType quote('"');
    // create
    Latin1StringType escapedQuote("\"\"");
    T newString;
    newString.reserve(estimatedNewLength(string, addQuotes));
    if (addQuotes) {
        newString.append(quote);
    }
    for (int i = 0; i < string.length(); i++) {
        const CharType c = string.at(i);
        if (c == quote)
            newString.append(escapedQuote);
        else
            newString.append(c);
    }
    if (addQuotes) {
        newString.append(quote);
    }
    newString.squeeze();
    return newString;
}

static bool shouldAddQuotesToIdentifier(const QByteArray& string)
{
    return !string.isEmpty() && (!KDb::isIdentifier(string) || KDb::isKDbSqlKeyword(string));
}

QString KDb::escapeIdentifier(const QString& string)
{
    return ::escapeIdentifier<QString, QLatin1String, QLatin1Char, QChar>(
        string, shouldAddQuotesToIdentifier(string.toLatin1()));
}

QByteArray KDb::escapeIdentifier(const QByteArray& string)
{
    return ::escapeIdentifier<QByteArray, QByteArray, char, char>(
        string, shouldAddQuotesToIdentifier(string));
}

QString KDb::escapeIdentifierAndAddQuotes(const QString& string)
{
    return ::escapeIdentifier<QString, QLatin1String, QLatin1Char, QChar>(string, true);
}

QByteArray KDb::escapeIdentifierAndAddQuotes(const QByteArray& string)
{
    return ::escapeIdentifier<QByteArray, QByteArray, char, char>(string, true);
}

QString KDb::escapeString(const QString& string)
{
    const QLatin1Char quote('\'');
    // find out the length ot the destination string
    // create
    QString newString(quote);
    newString.reserve(estimatedNewLength(string, true));
    for (int i = 0; i < string.length(); i++) {
        const QChar c = string.at(i);
        const ushort unicode = c.unicode();
        if (unicode == quote)
            newString.append(QLatin1String("''"));
        else if (unicode == '\t')
            newString.append(QLatin1String("\\t"));
        else if (unicode == '\\')
            newString.append(QLatin1String("\\\\"));
        else if (unicode == '\n')
            newString.append(QLatin1String("\\n"));
        else if (unicode == '\r')
            newString.append(QLatin1String("\\r"));
        else if (unicode == '\0')
            newString.append(QLatin1String("\\0"));
        else
            newString.append(c);
    }
    newString.append(QLatin1Char(quote));
    return newString;
}

KDbEscapedString KDb::escapeString(KDbDriver *drv, const QString& string)
{
    return drv ? drv->escapeString(string) : KDbEscapedString(KDb::escapeString(string));
}

KDbEscapedString KDb::escapeString(KDbConnection *conn, const QString& string)
{
    return conn ? conn->escapeString(string) : KDbEscapedString(KDb::escapeString(string));
}

//! @see handleHex()
const int CODE_POINT_DIGITS = std::numeric_limits<int>::max();
//! @see handleHex()
const int MAX_CODE_POINT_VALUE = 0x10FFFF;

//! @internal Decodes hex of length @a digits for handleXhh(), handleUxxxx() and handleUcodePoint()
//! If @a digits is CODE_POINT_DIGITS, any number of hex digits is decoded until '}' character
//! is found (error if not found), and the function succeeds when the resulting number
//! is not larger than MAX_CODE_POINT_VALUE.
//! If @a digits is smaller than CODE_POINT_DIGITS the function succeeds only if exactly @a digits
//! number of digits has been found.
//! @return -1 on error (when invalid character found or on missing character
//! or if the resulting number is too large)
//! @see KDb::unescapeString()
static int handleHex(QString *result, int *from, int stringLen, int *errorPosition, int digits)
{
    int digit = 0;
    for (int i=0; i<digits; ++i) {
        if ((*from + 1) >=  stringLen) { // unfinished
            if (errorPosition) {
                *errorPosition = *from;
            }
            return -1;
        }
        ++(*from);
        if (digits == CODE_POINT_DIGITS && (*result)[*from] == QLatin1Char('}')) {
            // special case: code point character decoded
            if (i == 0) {
                if (errorPosition) {
                    *errorPosition = *from;
                }
                return -1;
            }
            return digit;
        }
        const unsigned char d = hexDigitToInt((*result)[*from].toLatin1());
        if (d == 0xFF) { // unfinished or wrong character
            if (errorPosition) {
                *errorPosition = *from;
            }
            return -1;
        }
        digit = (digit << 4) + d;
        if (digits == CODE_POINT_DIGITS) {
            if (digit > MAX_CODE_POINT_VALUE) { // special case: exceeded limit of code point
                if (errorPosition) {
                    *errorPosition = *from;
                }
                return -1;
            }
        }
    }
    return digit;
}

//! @internal Handles \xhh format for handleEscape()
//! Assumption: the @a *from points to "x" in the "\x"
//! @see KDb::unescapeString()
static bool handleXhh(QString *result, int *from, int to, int stringLen, int *errorPosition)
{
    const int intDigit = handleHex(result, from, stringLen, errorPosition, 2);
    if (intDigit == -1) {
        return false;
    }
    (*result)[to] = QChar(static_cast<unsigned char>(intDigit), 0);
    return true;
}

//! @internal Handles \uxxxx format for handleEscape()
//! Assumption: the @a *from points to the "u" in the "\u".
//! @see KDb::unescapeString()
static bool handleUxxxx(QString *result, int *from, int to, int stringLen, int *errorPosition)
{
    const int intDigit = handleHex(result, from, stringLen, errorPosition, 4);
    if (intDigit == -1) {
        return false;
    }
    (*result)[to] = QChar(static_cast<unsigned short>(intDigit));
    return true;
}

//! @internal Handles \u{xxxxxx} format for handleEscape()
//! Assumption: the @a *from points to the "{" in the "\u{".
//! @see KDb::unescapeString()
static bool handleUcodePoint(QString *result, int *from, int to, int stringLen, int *errorPosition)
{
    const int intDigit = handleHex(result, from, stringLen, errorPosition, CODE_POINT_DIGITS);
    if (intDigit == -1) {
        return false;
    }
    (*result)[to] = QChar(intDigit);
    return true;
}

//! @internal Handles escaped character @a c2 for KDb::unescapeString()
//! Updates @a result
//! @return true on success
static bool handleEscape(QString *result, int *from, int *to, int stringLen, int *errorPosition)
{
    const QCharRef c2 = (*result)[*from];
    if (c2 == QLatin1Char('x')) { // \xhh
        if (!handleXhh(result, from, *to, stringLen, errorPosition)) {
            return false;
        }
    } else if (c2 == QLatin1Char('u')) { // \u
        if ((*from + 1) >=  stringLen) { // unfinished
            if (errorPosition) {
                *errorPosition = *from;
            }
            return false;
        }
        ++(*from);
        const QCharRef c3 = (*result)[*from];
        if (c3 == QLatin1Char('{')) { // \u{
            if (!handleUcodePoint(result, from, *to, stringLen, errorPosition)) {
                return false;
            }
        } else {
            --(*from);
            if (!handleUxxxx(result, from, *to, stringLen, errorPosition)) {
                return false;
            }
        }
#define _RULE(in, out) \
    } else if (c2 == QLatin1Char(in)) { \
        (*result)[*to] = QLatin1Char(out);
    _RULE('0', '\0') _RULE('b', '\b') _RULE('f', '\f') _RULE('n', '\n')
    _RULE('r', '\r') _RULE('t', '\t') _RULE('v', '\v')
#undef _RULE
    } else { // \ ' " ? % _ and any other without special meaning can be escaped: just skip "\"
        (*result)[*to] = c2;
    }
    return true;
}

QString KDb::unescapeString(const QString& string, char quote, int *errorPosition)
{
    if (quote != '\'' && quote != '\"') {
        if (errorPosition) {
            *errorPosition = 0;
        }
        return QString();
    }
    const QLatin1Char quoteChar(quote);
    if (string.isEmpty()
        || (!string.contains(QLatin1Char('\\')) && !string.contains(quoteChar)))
    {
        if (errorPosition) {
            *errorPosition = -1;
        }
        return string; // optimization: there are no escapes and quotes
    }
    QString result(string);
    const int stringLen = string.length();
    int from = 0;
    int to = 0;
    bool doubleQuoteExpected = false;
    while (from < stringLen) {
        const QCharRef c = result[from];
        if (doubleQuoteExpected) {
            if (c == quoteChar) {
                result[to] = c;
                doubleQuoteExpected = false;
            } else {
                // error: missing second quote
                if (errorPosition) {
                    *errorPosition = from - 1; // -1 because error is at prev. char
                }
                return QString();
            }
        } else if (c == quoteChar) {
            doubleQuoteExpected = true;
            ++from;
            continue;
        } else if (c == QLatin1Char('\\')) { // escaping
            if ((from + 1) >=  stringLen) { // ignore unfinished '\'
                break;
            }
            ++from;
            if (!handleEscape(&result, &from, &to, stringLen, errorPosition)) {
                return QString();
            }
        } else { // normal character: skip
            result[to] = result[from];
        }
        ++from;
        ++to;
    }
    if (doubleQuoteExpected) { // error: string ends with a single quote
        if (errorPosition) {
            *errorPosition = from - 1;
        }
        return QString();
    }
    if (errorPosition) {
        *errorPosition = -1;
    }
    result.truncate(to);
    return result;
}

//! @return hex digit '0'..'F' for integer number 0..15
inline static char intToHexDigit(unsigned char val)
{
    return (val < 10) ? ('0' + val) : ('A' + (val - 10));
}

QString KDb::escapeBLOB(const QByteArray& array, BLOBEscapingType type)
{
    const int size = array.size();
    if (size == 0 && type == BLOBEscapingType::ZeroXHex)
        return QString();
    int escaped_length = size * 2;
    if (type == BLOBEscapingType::ZeroXHex || type == BLOBEscapingType::Octal)
        escaped_length += 2/*0x or X'*/;
    else if (type == BLOBEscapingType::XHex)
        escaped_length += 3; //X' + '
    else if (type == BLOBEscapingType::ByteaHex)
        escaped_length += (4 + 8); // E'\x + '::bytea

    QString str;
    str.reserve(escaped_length);
    if (str.capacity() < escaped_length) {
        kdbWarning() << "Not enough memory (cannot allocate" << escaped_length << "characters)";
        return QString();
    }
    if (type == BLOBEscapingType::XHex)
        str = QString::fromLatin1("X'");
    else if (type == BLOBEscapingType::ZeroXHex)
        str = QString::fromLatin1("0x");
    else if (type == BLOBEscapingType::Octal)
        str = QString::fromLatin1("'");
    else if (type == BLOBEscapingType::ByteaHex)
        str = QString::fromLatin1("E'\\\\x");

    if (type == BLOBEscapingType::Octal) {
        // only escape nonprintable characters as in Table 8-7:
        // https://www.postgresql.org/docs/8.1/interactive/datatype-binary.html
        // i.e. escape for bytes: < 32, >= 127, 39 ('), 92(\).
        for (int i = 0; i < size; i++) {
            const unsigned char val = array[i];
            if (val < 32 || val >= 127 || val == 39 || val == 92) {
                str.append(QLatin1Char('\\'));
                str.append(QLatin1Char('\\'));
                str.append(QChar::fromLatin1('0' + val / 64));
                str.append(QChar::fromLatin1('0' + (val % 64) / 8));
                str.append(QChar::fromLatin1('0' + val % 8));
            } else {
                str.append(QChar::fromLatin1(val));
            }
        }
    } else {
        for (int i = 0; i < size; i++) {
            const unsigned char val = array[i];
            str.append(QChar::fromLatin1(intToHexDigit(val / 16)));
            str.append(QChar::fromLatin1(intToHexDigit(val % 16)));
        }
    }
    if (type == BLOBEscapingType::XHex || type == BLOBEscapingType::Octal) {
        str.append(QLatin1Char('\''));
    } else if (type == BLOBEscapingType::ByteaHex) {
        str.append(QLatin1String("\'::bytea"));
    }
    return str;
}

QByteArray KDb::pgsqlByteaToByteArray(const char* data, int length)
{
    if (!data) {
        return QByteArray();
    }
    QByteArray array;
    int output = 0;
    if (length < 0) {
        length = qstrlen(data);
    }
    for (int pass = 0; pass < 2; pass++) {//2 passes to avoid allocating buffer twice:
        //  0: count #of chars; 1: copy data
        const char* s = data;
        const char* end = s + length;
        if (pass == 1) {
            //kdbDebug() << "processBinaryData(): real size == " << output;
            array.resize(output);
            output = 0;
        }
        for (int input = 0; s < end; output++) {
            //  kdbDebug()<<(int)s[0]<<" "<<(int)s[1]<<" "<<(int)s[2]<<" "<<(int)s[3]<<" "<<(int)s[4];
            if (s[0] == '\\' && (s + 1) < end) {
                //special cases as in https://www.postgresql.org/docs/8.1/interactive/datatype-binary.html
                if (s[1] == '\'') {// \'
                    if (pass == 1)
                        array[output] = '\'';
                    s += 2;
                } else if (s[1] == '\\') { // 2 backslashes
                    if (pass == 1)
                        array[output] = '\\';
                    s += 2;
                } else if ((input + 3) < length) {// \\xyz where xyz are 3 octal digits
                    if (pass == 1)
                        array[output] = char((int(s[1] - '0') * 8 + int(s[2] - '0')) * 8 + int(s[3] - '0'));
                    s += 4;
                } else {
                    kdbWarning() << "Missing octal value after backslash";
                    s++;
                }
            } else {
                if (pass == 1)
                    array[output] = s[0];
                s++;
            }
            //  kdbDebug()<<output<<": "<<(int)array[output];
        }
    }
    return array;
}

QByteArray KDb::xHexToByteArray(const char* data, int length, bool *ok)
{
    if (length < 0) {
        length = qstrlen(data);
    }
    if (length < 3 || data[0] != 'X' || data[1] != '\'' || data[length-1] != '\'') { // must be at least X''
        if (ok) {
            *ok = false;
        }
        return QByteArray();
    }
    data += 2; // eat X'
    length -= 3; // eax X' and '
    QByteArray array;
    if (!hexToByteArrayInternal(data, length, &array)) {
        if (ok) {
            *ok = false;
        }
        array.clear();
    }
    if (ok) {
        *ok = true;
    }
    return array;
}

/*! \return byte array converted from \a data of length \a length.
 \a data is escaped in format 0x*, where * is one or more bytes in hexadecimal format.
 See BLOBEscapingType::ZeroXHex. */
QByteArray KDb::zeroXHexToByteArray(const char* data, int length, bool *ok)
{
    if (length < 0) {
        length = qstrlen(data);
    }
    if (length < 3 || data[0] != '0' || data[1] != 'x') { // must be at least 0xD
        if (ok) {
            *ok = false;
        }
        return QByteArray();
    }
    data += 2; // eat 0x
    length -= 2;
    QByteArray array;
    if (!hexToByteArrayInternal(data, length, &array)) {
        if (ok) {
            *ok = false;
        }
        array.clear();
    }
    if (ok) {
        *ok = true;
    }
    return array;
}

QList<int> KDb::stringListToIntList(const QStringList &list, bool *ok)
{
    QList<int> result;
    foreach (const QString &item, list) {
        int val = item.toInt(ok);
        if (ok && !*ok) {
            return QList<int>();
        }
        result.append(val);
    }
    if (ok) {
        *ok = true;
    }
    return result;
}

// Based on KConfigGroupPrivate::serializeList() from kconfiggroup.cpp (kdelibs 4)
QString KDb::serializeList(const QStringList &list)
{
    QString value;

    if (!list.isEmpty()) {
        QStringList::ConstIterator it = list.constBegin();
        const QStringList::ConstIterator end = list.constEnd();

        value = QString(*it).replace(QLatin1Char('\\'), QLatin1String("\\\\"))
                            .replace(QLatin1Char(','), QLatin1String("\\,"));

        while (++it != end) {
            // In the loop, so it is not done when there is only one element.
            // Doing it repeatedly is a pretty cheap operation.
            value.reserve(4096);

            value += QLatin1Char(',')
                     + QString(*it).replace(QLatin1Char('\\'), QLatin1String("\\\\"))
                                   .replace(QLatin1Char(','), QLatin1String("\\,"));
        }

        // To be able to distinguish an empty list from a list with one empty element.
        if (value.isEmpty())
            value = QLatin1String("\\0");
    }

    return value;
}

// Based on KConfigGroupPrivate::deserializeList() from kconfiggroup.cpp (kdelibs 4)
QStringList KDb::deserializeList(const QString &data)
{
    if (data.isEmpty())
        return QStringList();
    if (data == QLatin1String("\\0"))
        return QStringList(QString());
    QStringList value;
    QString val;
    val.reserve(data.size());
    bool quoted = false;
    for (int p = 0; p < data.length(); p++) {
        if (quoted) {
            val += data[p];
            quoted = false;
        } else if (data[p].unicode() == QLatin1Char('\\')) {
            quoted = true;
        } else if (data[p].unicode() == QLatin1Char(',')) {
            val.squeeze(); // release any unused memory
            value.append(val);
            val.clear();
            val.reserve(data.size() - p);
        } else {
            val += data[p];
        }
    }
    value.append(val);
    return value;
}

QList<int> KDb::deserializeIntList(const QString &data, bool *ok)
{
    return KDb::stringListToIntList(
        KDb::deserializeList(data), ok);
}

QString KDb::variantToString(const QVariant& v)
 {
    if (v.type() == QVariant::ByteArray) {
        return KDb::escapeBLOB(v.toByteArray(), KDb::BLOBEscapingType::Hex);
    }
    else if (v.type() == QVariant::StringList) {
        return serializeList(v.toStringList());
    }
    return v.toString();
}

QVariant KDb::stringToVariant(const QString& s, QVariant::Type type, bool* ok)
{
    if (s.isNull()) {
        if (ok)
            *ok = true;
        return QVariant();
    }
    switch (type) {
    case QVariant::Invalid:
        if (ok)
            *ok = false;
        return QVariant();
    case QVariant::ByteArray: {//special case: hex string
        const int len = s.length();
        QByteArray ba;
        ba.resize(len / 2 + len % 2);
        for (int i = 0; i < (len - 1); i += 2) {
            bool _ok;
            int c = s.midRef(i, 2).toInt(&_ok, 16);
            if (!_ok) {
                if (ok)
                    *ok = _ok;
                kdbWarning() << "Error in digit" << i;
                return QVariant();
            }
            ba[i/2] = (char)c;
        }
        if (ok)
            *ok = true;
        return ba;
    }
    case QVariant::StringList:
        *ok = true;
        return KDb::deserializeList(s);
    default:;
    }

    QVariant result(s);
    if (!result.convert(type)) {
        if (ok)
            *ok = false;
        return QVariant();
    }
    if (ok)
        *ok = true;
    return result;
}

bool KDb::isDefaultValueAllowed(const KDbField &field)
{
    return !field.isUniqueKey();
}

void KDb::getLimitsForFieldType(KDbField::Type type, qlonglong *minValue, qlonglong *maxValue,
                                Signedness signedness)
{
    if (!minValue || !maxValue) {
        return;
    }
    switch (type) {
    case KDbField::Byte:
//! @todo always ok?
        *minValue = signedness == KDb::Signed ? -0x80 : 0;
        *maxValue = signedness == KDb::Signed ? 0x7F : 0xFF;
        break;
    case KDbField::ShortInteger:
        *minValue = signedness == KDb::Signed ? -0x8000 : 0;
        *maxValue = signedness == KDb::Signed ? 0x7FFF : 0xFFFF;
        break;
    case KDbField::Integer:
    case KDbField::BigInteger: //!< @todo cannot return anything larger?
    default:
        *minValue = signedness == KDb::Signed ? qlonglong(-0x07FFFFFFF) : qlonglong(0);
        *maxValue = signedness == KDb::Signed ? qlonglong(0x07FFFFFFF) : qlonglong(0x0FFFFFFFF);
    }
}

KDbField::Type KDb::maximumForIntegerFieldTypes(KDbField::Type t1, KDbField::Type t2)
{
    if (!KDbField::isIntegerType(t1) || !KDbField::isIntegerType(t2))
        return KDbField::InvalidType;
    if (t1 == t2)
        return t2;
    if (t1 == KDbField::ShortInteger && t2 != KDbField::Integer && t2 != KDbField::BigInteger)
        return t1;
    if (t1 == KDbField::Integer && t2 != KDbField::BigInteger)
        return t1;
    if (t1 == KDbField::BigInteger)
        return t1;
    return KDb::maximumForIntegerFieldTypes(t2, t1); //swap
}

QString KDb::simplifiedFieldTypeName(KDbField::Type type)
{
    if (KDbField::isNumericType(type))
        return KDbField::tr("Number"); //simplify
    else if (type == KDbField::BLOB)
//! @todo support names of other BLOB subtypes
        return KDbField::tr("Image"); //simplify

    return KDbField::typeGroupName(KDbField::typeGroup(type));
}

QString KDb::defaultFileBasedDriverMimeType()
{
    return QLatin1String("application/x-kexiproject-sqlite3");
}

QString KDb::defaultFileBasedDriverId()
{
    return QLatin1String("org.kde.kdb.sqlite");
}

// Try to convert from string to type T
template <typename T>
QVariant convert(T (QString::*ConvertToT)(bool*,int) const, const char *data, int size,
                 qlonglong minValue, qlonglong maxValue, bool *ok)
{
    T v = (QString::fromLatin1(data, size).*ConvertToT)(ok, 10);
    if (*ok) {
        *ok = minValue <= v && v <= maxValue;
    }
    return KDb::iif(*ok, QVariant(v));
}

QVariant KDb::cstringToVariant(const char* data, KDbField::Type type, bool *ok, int length,
                               KDb::Signedness signedness)
{
    bool tempOk;
    bool *thisOk = ok ? ok : &tempOk;
    if (type < KDbField::Byte || type > KDbField::LastType) {
        *thisOk = false;
        return QVariant();
    }
    if (!data) { // NULL value
        *thisOk = true;
        return QVariant();
    }
    // from most to least frequently used types:

    if (KDbField::isTextType(type)) {
        *thisOk = true;
        //! @todo use KDbDriverBehavior::TEXT_TYPE_MAX_LENGTH for Text type?
        return QString::fromUtf8(data, length);
    }
    if (KDbField::isIntegerType(type)) {
        qlonglong minValue, maxValue;
        const bool isUnsigned = signedness == KDb::Unsigned;
        KDb::getLimitsForFieldType(type, &minValue, &maxValue, signedness);
        switch (type) {
        case KDbField::Byte: // Byte here too, minValue/maxValue will take care of limits
        case KDbField::ShortInteger:
            return isUnsigned ?
                convert(&QString::toUShort, data, length, minValue, maxValue, thisOk)
                : convert(&QString::toShort, data, length, minValue, maxValue, thisOk);
        case KDbField::Integer:
            return isUnsigned ?
                convert(&QString::toUInt, data, length, minValue, maxValue, thisOk)
                : convert(&QString::toInt, data, length, minValue, maxValue, thisOk);
        case KDbField::BigInteger:
            return convert(&QString::toLongLong, data, length, minValue, maxValue, thisOk);
        default:
            qFatal("Unsupported integer type %d", type);
        }
    }
    if (KDbField::isFPNumericType(type)) {
        const QVariant result(QString::fromLatin1(data, length).toDouble(thisOk));
        return KDb::iif(*thisOk, result);
    }
    if (type == KDbField::BLOB) {
        *thisOk = length >= 0;
        return *thisOk ? QVariant(QByteArray(data, length)) : QVariant();
    }
    // the default
//! @todo date/time?
    QVariant result(QString::fromUtf8(data, length));
    if (!result.convert(KDbField::variantType(type))) {
        *thisOk = false;
        return QVariant();
    }
    *thisOk = true;
    return result;
}

QStringList KDb::libraryPaths()
{
    QStringList result;
    foreach (const QString& path, qApp->libraryPaths()) {
        const QString dir(path + QLatin1Char('/') + QLatin1String(KDB_BASE_NAME_LOWER));
        if (QDir(dir).exists() && QDir(dir).isReadable()) {
            result += dir;
        }
    }
    return result;
}

QString KDb::temporaryTableName(KDbConnection *conn, const QString &baseName)
{
    if (!conn) {
        return QString();
    }
    while (true) {
        QString name = QLatin1String("tmp__") + baseName;
        for (int i = 0; i < 10; ++i) {
            name += QString::number(int(double(qrand()) / RAND_MAX * 0x10), 16);
        }
        const tristate res = conn->containsTable(name);
        if (~res) {
            return QString();
        } else if (res == false) {
            return name;
        }
    }
}

QString KDb::sqlite3ProgramPath()
{
    QString path = KDbUtils::findExe(QLatin1String("sqlite3"));
    if (path.isEmpty()) {
        kdbWarning() << "Could not find program \"sqlite3\"";
    }
    return path;
}

bool KDb::importSqliteFile(const QString &inputFileName, const QString &outputFileName)
{
    const QString sqlite_app = KDb::sqlite3ProgramPath();
    if (sqlite_app.isEmpty()) {
        return false;
    }

    QFileInfo fi(inputFileName);
    if (!fi.isReadable()) {
        kdbWarning() << "No readable input file" << fi.absoluteFilePath();
        return false;
    }
    QFileInfo fo(outputFileName);
    if (QFile(fo.absoluteFilePath()).exists()) {
        if (!QFile::remove(fo.absoluteFilePath())) {
            kdbWarning() << "Could not remove output file" << fo.absoluteFilePath();
            return false;
        }
    }
    kdbDebug() << inputFileName << fi.absoluteDir().path() << fo.absoluteFilePath();

    QProcess p;
    p.start(sqlite_app, QStringList() << fo.absoluteFilePath());
    if (!p.waitForStarted()) {
        kdbWarning() << "Failed to start program" << sqlite_app;
        return false;
    }
    QByteArray line(".read " + QFile::encodeName(fi.absoluteFilePath()));
    if (p.write(line) != line.length() || !p.waitForBytesWritten()) {
        kdbWarning() << "Failed to send \".read\" command to program" << sqlite_app;
        return false;
    }
    p.closeWriteChannel();
    if (!p.waitForFinished()) {
        kdbWarning() << "Failed to finish program" << sqlite_app;
        return false;
    }
    return true;
}

//---------

bool KDb::isIdentifier(const QString& s)
{
    int i;
    const int sLength = s.length();
    for (i = 0; i < sLength; i++) {
        const char c = s.at(i).toLower().toLatin1();
        if (c == 0 || !(c == '_' || (c >= 'a' && c <= 'z') || (i > 0 && c >= '0' && c <= '9')))
            break;
    }
    return i > 0 && i == sLength;
}

bool KDb::isIdentifier(const QByteArray& s)
{
    int i;
    const int sLength = s.length();
    for (i = 0; i < sLength; i++) {
        const char c = s.at(i);
        if (c == 0 || !(c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (i > 0 && c >= '0' && c <= '9'))) {
            break;
        }
    }
    return i > 0 && i == sLength;
}

static inline QString charToIdentifier(const QChar& c)
{
    if (c.unicode() >= TRANSLITERATION_TABLE_SIZE)
        return QLatin1String("_");
    const char *const s = transliteration_table[c.unicode()];
    return s ? QString::fromLatin1(s) : QLatin1String("_");
}

QString KDb::stringToIdentifier(const QString &s)
{
    if (s.isEmpty())
        return QString();
    QString r, id = s.simplified();
    if (id.isEmpty())
        return QString();
    r.reserve(id.length());
    id.replace(QLatin1Char(' '), QLatin1String("_"));
    const QChar c = id[0];
    const char ch = c.toLatin1();
    QString add;
    bool wasUnderscore = false;

    if (ch >= '0' && ch <= '9') {
        r += QLatin1Char('_') + c;
    } else {
        add = charToIdentifier(c);
        r += add;
        wasUnderscore = add == QLatin1String("_");
    }

    const int idLength = id.length();
    for (int i = 1; i < idLength; i++) {
        add = charToIdentifier(id.at(i));
        if (wasUnderscore && add == QLatin1String("_"))
            continue;
        wasUnderscore = add == QLatin1String("_");
        r += add;
    }
    return r;
}

QString KDb::identifierExpectedMessage(const QString &valueName, const QVariant& v)
{
    return QLatin1String("<p>") + kdb::tr("Value of \"%1\" field must be an identifier.")
            .arg(valueName)
           + QLatin1String("</p><p>")
           + kdb::tr("\"%1\" is not a valid identifier.").arg(v.toString()) + QLatin1String("</p>");
}

//---------

KDbEscapedString KDb::valueToSql(KDbField::Type ftype, const QVariant& v)
{
    return valueToSqlInternal(nullptr, ftype, v);
}

static QByteArray dateToSqlInternal(const QVariant& v, bool allowInvalidKDbDate)
{
    QByteArray result(QByteArrayLiteral("<INVALID_DATE>"));
    if (v.canConvert<KDbDate>()) {
        const KDbDate date(v.value<KDbDate>());
        if (date.isValid() || allowInvalidKDbDate) {
            result = date.toString(); // OK even if invalid or null
        }
    } else if (v.canConvert<QDate>()) {
        const QDate date(v.toDate());
        if (date.isValid()) {
            result = date.toString(Qt::ISODate).toLatin1();
        }
    }
    return result;
}

KDbEscapedString KDb::dateToSql(const QVariant& v)
{
    return KDbEscapedString('#') + dateToSqlInternal(v, true) + '#';
}

static QByteArray timeToSqlInternal(const QVariant& v, bool allowInvalidKDbTime)
{
    QByteArray result(QByteArrayLiteral("<INVALID_TIME>"));
    if (v.canConvert<KDbTime>()) {
        const KDbTime time(v.value<KDbTime>());
        if (time.isValid() || allowInvalidKDbTime) {
            result = time.toString(); // OK even if invalid or null
        }
    } else if (v.canConvert<QTime>()) {
        const QTime time(v.toTime());
        if (time.isValid()) {
            if (time.msec() == 0) {
                result = time.toString(Qt::ISODate).toLatin1();
            } else {
                result = KDbUtils::toISODateStringWithMs(time).toLatin1();
            }
        }
    }
    return result;
}

KDbEscapedString KDb::timeToSql(const QVariant& v)
{
    return KDbEscapedString('#') + timeToSqlInternal(v, true) + '#';
}

static QByteArray dateTimeToSqlInternal(const QVariant& v, char separator, bool allowInvalidKDbDateTime)
{
    QByteArray result(QByteArrayLiteral("<INVALID_DATETIME>"));
    if (v.canConvert<KDbDateTime>()) {
        const KDbDateTime dateTime(v.value<KDbDateTime>());
        if (dateTime.isValid() || allowInvalidKDbDateTime) {
            result = dateTime.toString(); // OK even if invalid or null
        }
    } else if (v.canConvert<QDateTime>()) {
        const QDateTime dateTime(v.toDateTime());
        if (dateTime.isValid()) {
            result = dateTime.date().toString(Qt::ISODate).toLatin1() + separator;
            const QTime time(dateTime.time());
            if (time.msec() == 0) {
                result += time.toString(Qt::ISODate).toLatin1();
            } else {
                result += KDbUtils::toISODateStringWithMs(time).toLatin1();
            }
        }
    }
    return result;
}

KDbEscapedString KDb::dateTimeToSql(const QVariant& v)
{
    return KDbEscapedString('#') + dateTimeToSqlInternal(v, ' ', true) + '#';
}

KDbEscapedString KDb::dateTimeToSql(const QDateTime& v)
{
    return KDb::dateTimeToIsoString(v);
}

KDbEscapedString KDb::dateToIsoString(const QVariant& v)
{
    return KDbEscapedString('\'') + dateToSqlInternal(v, false) + KDbEscapedString('\'');
}

KDbEscapedString KDb::timeToIsoString(const QVariant& v)
{
    return KDbEscapedString('\'') + timeToSqlInternal(v, false) + KDbEscapedString('\'');
}

KDbEscapedString KDb::dateTimeToIsoString(const QVariant& v)
{
    return KDbEscapedString('\'') + dateTimeToSqlInternal(v, 'T', false) + KDbEscapedString('\'');
}

//--------------------------------------------------------------------------------

#ifdef KDB_DEBUG_GUI

static KDb::DebugGUIHandler s_debugGUIHandler = nullptr;

void KDb::setDebugGUIHandler(KDb::DebugGUIHandler handler)
{
    s_debugGUIHandler = handler;
}

void KDb::debugGUI(const QString& text)
{
    if (s_debugGUIHandler)
        s_debugGUIHandler(text);
}

static KDb::AlterTableActionDebugGUIHandler s_alterTableActionDebugHandler = nullptr;

void KDb::setAlterTableActionDebugHandler(KDb::AlterTableActionDebugGUIHandler handler)
{
    s_alterTableActionDebugHandler = handler;
}

void KDb::alterTableActionDebugGUI(const QString& text, int nestingLevel)
{
    if (s_alterTableActionDebugHandler)
        s_alterTableActionDebugHandler(text, nestingLevel);
}

#endif // KDB_DEBUG_GUI

#include "KDb.moc"
