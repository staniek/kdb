/* This file is part of the KDE project
   Copyright (C) 2005-2012 Jarosław Staniek <staniek@kde.org>

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

#ifndef KDB_SQLITEPREPAREDSTATEMENT_H
#define KDB_SQLITEPREPAREDSTATEMENT_H

#include "KDbPreparedStatementInterface.h"
#include "SqliteConnection_p.h"

class KDbField;

/*! Implementation of prepared statements for the SQLite driver. */
class SqlitePreparedStatement : public KDbPreparedStatementInterface, public SqliteConnectionInternal
{
public:
    explicit SqlitePreparedStatement(SqliteConnectionInternal* conn);

    virtual ~SqlitePreparedStatement();

protected:
    virtual bool prepare(const KDbEscapedString& sql);

    virtual bool execute(
        KDbPreparedStatement::Type type,
        const KDbField::List& selectFieldList,
        KDbFieldList* insertFieldList,
        const KDbPreparedStatementParameters& parameters);

    bool bindValue(KDbField *field, const QVariant& value, int arg);

    sqlite3_stmt *m_handle;
};

#endif
