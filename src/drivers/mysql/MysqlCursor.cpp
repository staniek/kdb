/* This file is part of the KDE project
   Copyright (C) 2003 Joseph Wenninger<jowenn@kde.org>
   Copyright (C) 2005-2010 Jarosław Staniek <staniek@kde.org>

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

#include "MysqlCursor.h"
#include "MysqlConnection.h"
#include "MysqlConnection_p.h"
#include <Predicate/Error.h>
#include <Predicate/Utils.h>

#include <QtDebug>
#include <limits.h>

#define BOOL bool

using namespace Predicate;

MysqlCursor::MysqlCursor(Predicate::Connection* conn, const QString& statement, uint cursor_options)
        : Cursor(conn, statement, cursor_options)
        , d(new MysqlCursorData(conn))
{
    m_options |= Buffered;
    d->mysql = static_cast<MysqlConnection*>(conn)->d->mysql;
// PreDrvDbg << "constructor for query statement";
}

MysqlCursor::MysqlCursor(Connection* conn, QuerySchema* query, uint options)
        : Cursor(conn, query, options)
        , d(new MysqlCursorData(conn))
{
    m_options |= Buffered;
    d->mysql = static_cast<MysqlConnection*>(conn)->d->mysql;
// PreDrvDbg << "constructor for query statement";
}

MysqlCursor::~MysqlCursor()
{
    close();
}

bool MysqlCursor::drv_open(const QString& sql)
{
    const QByteArray st(sql.toUtf8());
    if (mysql_real_query(d->mysql, st, st.length()) == 0) {
        if (mysql_errno(d->mysql) == 0) {
            d->mysqlres = mysql_store_result(d->mysql);
            m_fieldCount = mysql_num_fields(d->mysqlres);
            m_fieldsToStoreInRecord = m_fieldCount;
            d->numRows = mysql_num_rows(d->mysqlres);
            m_at = 0;

            m_opened = true;
            m_records_in_buf = d->numRows;
            m_buffering_completed = true;
            m_afterLast = false;
            return true;
        }
    }

    d->storeResult();
    return false;
}

bool MysqlCursor::drv_close()
{
    mysql_free_result(d->mysqlres);
    d->mysqlres = 0;
    d->mysqlrow = 0;
//js: done in superclass: m_numFields=0;
    d->lengths = 0;
    m_opened = false;
    d->numRows = 0;
    return true;
}

/*bool MysqlCursor::drv_moveFirst() {
  return true; //TODO
}*/

void MysqlCursor::drv_getNextRecord()
{
// PreDrvDbg;
    if (at() < d->numRows && at() >= 0) {
        d->lengths = mysql_fetch_lengths(d->mysqlres);
        m_fetchResult = FetchOK;
    } else if (at() >= d->numRows) {
        m_fetchResult = FetchEnd;
    } else {
        // control will reach here only when at() < 0 ( which is usually -1 )
        // -1 is same as "1 beyond the End"
        m_fetchResult = FetchEnd;
    }
}

// This isn't going to work right now as it uses d->mysqlrow
QVariant MysqlCursor::value(uint pos)
{
    if (!d->mysqlrow || pos >= m_fieldCount || d->mysqlrow[pos] == 0)
        return QVariant();

    Predicate::Field *f = (m_fieldsExpanded && pos < (uint)m_fieldsExpanded->count())
                       ? m_fieldsExpanded->at(pos)->field : 0;

//! @todo js: use MYSQL_FIELD::type here!

    return Predicate::cstringToVariant(d->mysqlrow[pos], f, d->lengths[pos]);
    /* moved to cstringToVariant()
      //from most to least frequently used types:
      if (!f || f->isTextType())
        return QVariant( QString::fromUtf8((const char*)d->mysqlrow[pos], d->lengths[pos]) );
      else if (f->isIntegerType())
    //! @todo support BigInteger
        return QVariant( Q3CString((const char*)d->mysqlrow[pos], d->lengths[pos]).toInt() );
      else if (f->isFPNumericType())
        return QVariant( Q3CString((const char*)d->mysqlrow[pos], d->lengths[pos]).toDouble() );

      //default
      return QVariant(QString::fromUtf8((const char*)d->mysqlrow[pos], d->lengths[pos]));*/
}


/* As with sqlite, the DB library returns all values (including numbers) as
   strings. So just put that string in a QVariant and let Predicate deal with it.
 */
bool MysqlCursor::drv_storeCurrentRecord(RecordData* data) const
{
// PreDrvDbg << "position is " << (long)m_at;
    if (d->numRows <= 0)
        return false;

//! @todo js: use MYSQL_FIELD::type here!
//!           see SQLiteCursor::storeCurrentRecord()

    const uint fieldsExpandedCount = m_fieldsExpanded ? m_fieldsExpanded->count() : UINT_MAX;
    const uint realCount = qMin(fieldsExpandedCount, m_fieldsToStoreInRecord);
    for (uint i = 0; i < realCount; i++) {
        Field *f = m_fieldsExpanded ? m_fieldsExpanded->at(i)->field : 0;
        if (m_fieldsExpanded && !f)
            continue;
        (*data)[i] = Predicate::cstringToVariant(d->mysqlrow[i], f, d->lengths[i]);
        /* moved to cstringToVariant()
            if (f && f->type()==Field::BLOB) {
              data[i] = QByteArray(d->mysqlrow[i], d->mysqlres->lengths[i]);
              PreDbg << data[i].toByteArray().size();
            }
        //! @todo more types!
        //! @todo look at what type mysql declares!
            else {
              data[i] = QVariant(QString::fromUtf8((const char*)d->mysqlrow[i], d->lengths[i]));
            }*/
    }
    return true;
}

void MysqlCursor::drv_appendCurrentRecordToBuffer()
{
}


void MysqlCursor::drv_bufferMovePointerNext()
{
    d->mysqlrow = mysql_fetch_row(d->mysqlres);
    d->lengths = mysql_fetch_lengths(d->mysqlres);
}

void MysqlCursor::drv_bufferMovePointerPrev()
{
    //MYSQL_ROW_OFFSET ro=mysql_row_tell(d->mysqlres);
    mysql_data_seek(d->mysqlres, m_at - 1);
    d->mysqlrow = mysql_fetch_row(d->mysqlres);
    d->lengths = mysql_fetch_lengths(d->mysqlres);
}


void MysqlCursor::drv_bufferMovePointerTo(qint64 to)
{
    //MYSQL_ROW_OFFSET ro=mysql_row_tell(d->mysqlres);
    mysql_data_seek(d->mysqlres, to);
    d->mysqlrow = mysql_fetch_row(d->mysqlres);
    d->lengths = mysql_fetch_lengths(d->mysqlres);
}

const char** MysqlCursor::recordData() const
{
    //! @todo
    return 0;
}

void MysqlCursor::drv_clearServerResult()
{
}