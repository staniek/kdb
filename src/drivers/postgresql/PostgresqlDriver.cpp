/* This file is part of the KDE project
   Copyright (C) 2003 Adam Pigg <adam@piggz.co.uk>
   Copyright (C) 2010-2015 Jarosław Staniek <staniek@kde.org>

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

#include "PostgresqlDriver.h"

#include "KDbConnection.h"
#include "KDbDriverManager.h"
#include "KDbDriverBehavior.h"
#include "KDbExpression.h"
#include "KDb.h"

#include "PostgresqlConnection.h"

#include <KPluginFactory>

#include <libpq-fe.h>

K_PLUGIN_CLASS_WITH_JSON(PostgresqlDriver, "kdb_postgresqldriver.json")

PostgresqlDriver::PostgresqlDriver(QObject *parent, const QVariantList &args)
        : KDbDriver(parent, args)
{
    KDbDriverBehavior *beh = behavior();
    beh->features = SingleTransactions | CursorForward | CursorBackward;
//! @todo enable this when KDb supports multiple: beh->features = MultipleTransactions | CursorForward | CursorBackward;

    beh->UNSIGNED_TYPE_KEYWORD = QString();
    beh->ROW_ID_FIELD_NAME = QLatin1String("oid");
    beh->SPECIAL_AUTO_INCREMENT_DEF = false;
    beh->AUTO_INCREMENT_TYPE = QLatin1String("SERIAL");
    beh->AUTO_INCREMENT_FIELD_OPTION = QString();
    beh->AUTO_INCREMENT_PK_FIELD_OPTION = QLatin1String("PRIMARY KEY");
    beh->ALWAYS_AVAILABLE_DATABASE_NAME = QLatin1String("template1");
    beh->OPENING_QUOTATION_MARK_BEGIN_FOR_IDENTIFIER = '"';
    beh->CLOSING_QUOTATION_MARK_BEGIN_FOR_IDENTIFIER = '"';
    beh->LIKE_OPERATOR = QLatin1String("ILIKE");
    // Use SQL compliant TRUE or FALSE as described
    // at https://www.postgresql.org/docs/8.0/interactive/datatype-boolean.html
    // 1 or 0 does not work.
    beh->BOOLEAN_TRUE_LITERAL = QLatin1String("TRUE");
    beh->BOOLEAN_FALSE_LITERAL = QLatin1String("FALSE");
    beh->USE_TEMPORARY_DATABASE_FOR_CONNECTION_IF_NEEDED = true;
    beh->GET_TABLE_NAMES_SQL = KDbEscapedString(
        "SELECT table_name FROM information_schema.tables WHERE "
        "table_type='BASE TABLE' AND table_schema NOT IN ('pg_catalog', 'information_schema')");

    initDriverSpecificKeywords(m_keywords);
    initPgsqlToKDbMap();

    //predefined properties
    //https://www.postgresql.org/docs/9.5/static/libpq-misc.html#LIBPQ-PQLIBVERSION
//! @todo use QLibrary to resolve PQlibVersion
    beh->properties.insert("client_library_version", PQlibVersion());
    //! @todo pgsql default_server_encoding: should be a property of connection
    //beh->properties["default_server_encoding"] = QString();

    beh->typeNames[KDbField::Byte] = QLatin1String("SMALLINT");
    beh->typeNames[KDbField::ShortInteger] = QLatin1String("SMALLINT");
    beh->typeNames[KDbField::Integer] = QLatin1String("INTEGER");
    beh->typeNames[KDbField::BigInteger] = QLatin1String("BIGINT");
    beh->typeNames[KDbField::Boolean] = QLatin1String("BOOLEAN");
    beh->typeNames[KDbField::Date] = QLatin1String("DATE");
    beh->typeNames[KDbField::DateTime] = QLatin1String("TIMESTAMP");
    beh->typeNames[KDbField::Time] = QLatin1String("TIME");
    beh->typeNames[KDbField::Float] = QLatin1String("REAL");
    beh->typeNames[KDbField::Double] = QLatin1String("DOUBLE PRECISION");
    beh->typeNames[KDbField::Text] = QLatin1String("CHARACTER VARYING");
    beh->typeNames[KDbField::LongText] = QLatin1String("TEXT");
    beh->typeNames[KDbField::BLOB] = QLatin1String("BYTEA");
}

PostgresqlDriver::~PostgresqlDriver()
{
}

QString PostgresqlDriver::sqlTypeName(KDbField::Type type, const KDbField &field) const
{
    if (type == KDbField::Null) {
        return QLatin1String("NULL");
    }
    if (type == KDbField::Float || type == KDbField::Double) {
        if (field.precision() > 0) {
            return QLatin1String("NUMERIC");
        }
    }
    return KDbDriver::sqlTypeName(type, field);
}

KDbConnection* PostgresqlDriver::drv_createConnection(const KDbConnectionData& connData,
                                                      const KDbConnectionOptions &options)
{
    return new PostgresqlConnection(this, connData, options);
}

bool PostgresqlDriver::isSystemObjectName(const QString& name) const
{
    Q_UNUSED(name);
    return false;
}

bool PostgresqlDriver::drv_isSystemFieldName(const QString& name) const
{
    Q_UNUSED(name);
    return false;
}

bool PostgresqlDriver::isSystemDatabaseName(const QString& name) const
{
    return    0 == name.compare(QLatin1String("template1"), Qt::CaseInsensitive)
           || 0 == name.compare(QLatin1String("template0"), Qt::CaseInsensitive)
           || 0 == name.compare(QLatin1String("postgres"), Qt::CaseInsensitive);
}

KDbEscapedString PostgresqlDriver::escapeString(const QString& str) const
{
    //Cannot use libpq escape functions as they require a db connection
    //to escape using the char encoding of the database
    //see https://www.postgresql.org/docs/8.1/static/libpq-exec.html#LIBPQ-EXEC-ESCAPE-STRING
    return KDbEscapedString("E'")
           + KDbEscapedString(str).replace("\\", "\\\\").replace("'", "\\\'")
           + "'";
}

KDbEscapedString PostgresqlDriver::escapeString(const QByteArray& str) const
{
    //Cannot use libpq escape functions as they require a db connection
    //to escape using the char encoding of the database
    //see https://www.postgresql.org/docs/8.1/static/libpq-exec.html#LIBPQ-EXEC-ESCAPE-STRING
    return KDbEscapedString("'")
           + QByteArray(str).replace("\\", "\\\\").replace("'", "\\\'")
           + "'";
}

QString PostgresqlDriver::drv_escapeIdentifier(const QString& str) const
{
    return QString(str).replace(QLatin1Char('"'), QLatin1String("\"\""));
}

QByteArray PostgresqlDriver::drv_escapeIdentifier(const QByteArray& str) const
{
    return QByteArray(str).replace('"', "\"\"");
}

KDbEscapedString PostgresqlDriver::escapeBLOB(const QByteArray& array) const
{
    return KDbEscapedString(KDb::escapeBLOB(array, KDb::BLOBEscapingType::ByteaHex));
}

KDbEscapedString PostgresqlDriver::hexFunctionToString(const KDbNArgExpression &args,
                                                       KDbQuerySchemaParameterValueListIterator* params,
                                                       KDb::ExpressionCallStack* callStack) const
{
    Q_ASSERT(args.argCount() == 1);
    return KDbEscapedString("UPPER(ENCODE(%1, 'hex'))").arg(args.arg(0).toString(this, params, callStack));
}

KDbEscapedString PostgresqlDriver::ifnullFunctionToString(const KDbNArgExpression &args,
                                                          KDbQuerySchemaParameterValueListIterator* params,
                                                          KDb::ExpressionCallStack* callStack) const
{
    return KDbFunctionExpression::toString(QLatin1String("COALESCE"), this, args, params, callStack);
}

KDbEscapedString PostgresqlDriver::lengthFunctionToString(const KDbNArgExpression &args,
                                                          KDbQuerySchemaParameterValueListIterator* params,
                                                          KDb::ExpressionCallStack* callStack) const
{
    Q_ASSERT(args.argCount() == 1);
    if (args.arg(0).type() == KDbField::BLOB) {
        return KDbFunctionExpression::toString(QLatin1String("OCTET_LENGTH"), this, args, params, callStack);
    }
    return KDbDriver::lengthFunctionToString(args, params, callStack); // default
}

KDbEscapedString PostgresqlDriver::greatestOrLeastFunctionToString(const QString &name,
                                                const KDbNArgExpression &args,
                                                KDbQuerySchemaParameterValueListIterator* params,
                                                KDb::ExpressionCallStack* callStack) const
{
    return KDbFunctionExpression::greatestOrLeastFunctionUsingCaseToString(
                name, this, args, params, callStack);
}

KDbEscapedString PostgresqlDriver::unicodeFunctionToString(
                                                const KDbNArgExpression &args,
                                                KDbQuerySchemaParameterValueListIterator* params,
                                                KDb::ExpressionCallStack* callStack) const
{
    Q_ASSERT(args.argCount() == 1);
    return KDbEscapedString("ASCII(%1)").arg(args.arg(0).toString(this, params, callStack));
}

#include "PostgresqlDriver.moc"
