/* This file is part of the KDE project
   Copyright (C) 2003-2011 Jarosław Staniek <staniek@kde.org>

   Based on nexp.cpp : Parser module of Python-like language
   (C) 2001 Jarosław Staniek, MIMUW (www.mimuw.edu.pl)

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

#include "KDbExpression.h"
#include "KDb.h"
#include "KDbQuerySchema.h"
#include "KDbDriver.h"
#include "generated/sqlparser.h"

#include <ctype.h>

KDbQueryParameterExpressionData::KDbQueryParameterExpressionData()
 : KDbConstExpressionData()
 , m_type(KDbField::InvalidType)
{
    ExpressionDebug << "QueryParameterExpressionData" << ref;
}

KDbQueryParameterExpressionData::KDbQueryParameterExpressionData(
    KDbField::Type type, const QVariant& value)
 : KDbConstExpressionData(value)
 , m_type(type)
{
   ExpressionDebug << "QueryParameterExpressionData" << ref;
}

KDbQueryParameterExpressionData::~KDbQueryParameterExpressionData()
{
    ExpressionDebug << "~QueryParameterExpressionData" << ref;
}

KDbQueryParameterExpressionData* KDbQueryParameterExpressionData::clone()
{
    ExpressionDebug << "QueryParameterExpressionData::clone" << *this;
    return new KDbQueryParameterExpressionData(*this);
}

void KDbQueryParameterExpressionData::debugInternal(QDebug dbg, KDb::ExpressionCallStack* callStack) const
{
    Q_UNUSED(callStack);
    dbg.nospace() << QString::fromLatin1("QueryParExp([%1],type=%2)")
        .arg(value.toString()).arg(KDbDriver::defaultSQLTypeName(type()));
}

KDbEscapedString KDbQueryParameterExpressionData::toStringInternal(KDbQuerySchemaParameterValueListIterator* params,
                                                             KDb::ExpressionCallStack* callStack) const
{
    Q_UNUSED(callStack);
    return params ? params->getPreviousValueAsString(type())
           : KDbEscapedString("[%1]").arg(KDbEscapedString(value.toString()));
}

void KDbQueryParameterExpressionData::getQueryParameters(QList<KDbQuerySchemaParameter>& params)
{
    KDbQuerySchemaParameter param;
    param.message = value.toString();
    param.type = type();
    params.append(param);
}

bool KDbQueryParameterExpressionData::validateInternal(KDbParseInfo *parseInfo, KDb::ExpressionCallStack* callStack)
{
    Q_UNUSED(parseInfo);
    return typeInternal(callStack) != KDbField::InvalidType;
}

KDbField::Type KDbQueryParameterExpressionData::typeInternal(KDb::ExpressionCallStack* callStack) const
{
    Q_UNUSED(callStack);
    return m_type;
}

//=========================================

KDbQueryParameterExpression::KDbQueryParameterExpression()
 : KDbConstExpression(new KDbQueryParameterExpressionData)
{
    ExpressionDebug << "KDbQueryParameterExpression() ctor" << *this;
}

KDbQueryParameterExpression::KDbQueryParameterExpression(const QString& message)
        : KDbConstExpression(new KDbQueryParameterExpressionData(KDbField::Text, message),
              KDb::QueryParameterExpression, QUERY_PARAMETER)
{
}

KDbQueryParameterExpression::KDbQueryParameterExpression(const KDbQueryParameterExpression& expr)
        : KDbConstExpression(expr)
{
}

KDbQueryParameterExpression::KDbQueryParameterExpression(KDbExpressionData* data)
    : KDbConstExpression(data)
{
    ExpressionDebug << "KDbQueryParameterExpression ctor (KDbExpressionData*)" << *this;
}

KDbQueryParameterExpression::KDbQueryParameterExpression(const ExplicitlySharedExpressionDataPointer &ptr)
    : KDbConstExpression(ptr)
{
}

KDbQueryParameterExpression::~KDbQueryParameterExpression()
{
}

void KDbQueryParameterExpression::setType(KDbField::Type type)
{
    d->convert<KDbQueryParameterExpressionData>()->m_type = type;
}