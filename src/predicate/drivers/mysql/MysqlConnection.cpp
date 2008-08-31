/* This file is part of the KDE project
   Copyright (C) 2002 Lucijan Busch <lucijan@gmx.at>
                      Daniel Molkentin <molkentin@kde.org>
   Copyright (C) 2003 Joseph Wenninger<jowenn@kde.org>
   Copyright (C) 2004, 2006 Jarosław Staniek <staniek@kde.org>

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

#include <QVariant>
#include <QFile>
#include <QRegExp>
#include <QtDebug>

#include "MysqlDriver.h"
#include "MysqlConnection.h"
#include "MysqlConnection_p.h"
#include "MysqlCursor.h"
#include "MysqlPreparedStatement.h"
#include <Predicate/Error.h>


using namespace Predicate;

//--------------------------------------------------------------------------

MysqlConnection::MysqlConnection(Driver *driver, ConnectionData &conn_data)
        : Connection(driver, conn_data)
        , d(new MysqlConnectionInternal(this))
{
}

MysqlConnection::~MysqlConnection()
{
    destroy();
}

bool MysqlConnection::drv_connect(Predicate::ServerVersionInfo& version)
{
    const bool ok = d->db_connect(*data());
    if (!ok)
        return false;

    version.string = mysql_get_host_info(d->mysql);

    //retrieve server version info
#if 0 //this only works for client version >= 4.1 :(
    unsigned long v = mysql_get_server_version(d->mysql);
    // v - a number that represents the MySQL server version in this format
    // = major_version*10000 + minor_version *100 + sub_version
    version.major = v / 10000;
    version.minor = (v - version.major * 10000) / 100;
    version.release = v - version.major * 10000 - version.minor * 100;
#else //better way to get the version info: use 'version' built-in variable:
//! @todo this is hardcoded for now; define api for retrieving variables and use this API...
    QString versionString;
    const tristate res = querySingleString("SELECT @@version", versionString, /*column*/0, false /*!addLimitTo1*/);
    QRegExp versionRe("(\\d+)\\.(\\d+)\\.(\\d+)");
    if (res == true && versionRe.exactMatch(versionString)) { // (if querySingleString failed, the version will be 0.0.0...
        version.major = versionRe.cap(1).toInt();
        version.minor = versionRe.cap(2).toInt();
        version.release = versionRe.cap(3).toInt();
    }
#endif
    return true;
}

bool MysqlConnection::drv_disconnect()
{
    return d->db_disconnect();
}

Cursor* MysqlConnection::prepareQuery(const QString& statement, uint cursor_options)
{
    return new MysqlCursor(this, statement, cursor_options);
}

Cursor* MysqlConnection::prepareQuery(QuerySchema& query, uint cursor_options)
{
    return new MysqlCursor(this, query, cursor_options);
}

bool MysqlConnection::drv_getDatabasesList(QStringList &list)
{
    PreDrvDbg;
    list.clear();
    MYSQL_RES *res;

    if ((res = mysql_list_dbs(d->mysql, 0)) != 0) {
        MYSQL_ROW  row;
        while ((row = mysql_fetch_row(res)) != 0) {
            list << QString(row[0]);
        }
        mysql_free_result(res);
        return true;
    }

    d->storeResult();
// setError(ERR_DB_SPECIFIC,mysql_error(d->mysql));
    return false;
}

bool MysqlConnection::drv_createDatabase(const QString &dbName)
{
    PreDrvDbg << dbName;
    // mysql_create_db deprecated, use SQL here.
    if (drv_executeSQL("CREATE DATABASE " + (dbName)))
        return true;
    d->storeResult();
    return false;
}

bool MysqlConnection::drv_useDatabase(const QString &dbName, bool *cancelled, MessageHandler* msgHandler)
{
    Q_UNUSED(cancelled);
    Q_UNUSED(msgHandler);
//TODO is here escaping needed?
    return d->useDatabase(dbName);
}

bool MysqlConnection::drv_closeDatabase()
{
//TODO free resources
//As far as I know, mysql doesn't support that
    return true;
}

bool MysqlConnection::drv_dropDatabase(const QString &dbName)
{
//TODO is here escaping needed
    return drv_executeSQL("drop database " + dbName);
}

bool MysqlConnection::drv_executeSQL(const QString& statement)
{
    return d->executeSQL(statement);
}

quint64 MysqlConnection::drv_lastInsertRowID()
{
    //! @todo
    return (quint64)mysql_insert_id(d->mysql);
}

int MysqlConnection::serverResult()
{
    return d->res;
}

QString MysqlConnection::serverResultName()
{
    return QString();
}

void MysqlConnection::drv_clearServerResult()
{
    if (!d)
        return;
    d->res = 0;
}

QString MysqlConnection::serverErrorMsg()
{
    return d->errmsg;
}

bool MysqlConnection::drv_containsTable(const QString &tableName)
{
    bool success;
    return resultExists(QString("show tables like %1")
                        .arg(driver()->escapeString(tableName)), success) && success;
}

bool MysqlConnection::drv_getTablesList(QStringList &list)
{
    return queryStringList("show tables", list);
}

PreparedStatementInterface* MysqlConnection::prepareStatementInternal()
{
    return new MysqlPreparedStatement(*d);
}
