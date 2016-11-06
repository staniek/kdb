/* This file is part of the KDE project
   Copyright (C) 2003-2016 Jarosław Staniek <staniek@kde.org>

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

#include "KDbRelationship.h"
#include "KDbIndexSchema.h"
#include "KDbTableSchema.h"
#include "KDbQuerySchema.h"
#include "KDbDriver.h"
#include "kdb_debug.h"

KDbRelationship::KDbRelationship()
        : m_masterIndex(0)
        , m_detailsIndex(0)
        , m_masterIndexOwned(false)
        , m_detailsIndexOwned(false)
{
}

KDbRelationship::KDbRelationship(KDbIndexSchema* masterIndex, KDbIndexSchema* detailsIndex)
        : m_masterIndex(0)
        , m_detailsIndex(0)
        , m_masterIndexOwned(false)
        , m_detailsIndexOwned(false)
{
    (void)setIndices(masterIndex, detailsIndex);
}

KDbRelationship::KDbRelationship(KDbQuerySchema *query, KDbField *field1, KDbField *field2)
        : m_masterIndex(0)
        , m_detailsIndex(0)
        , m_masterIndexOwned(false)
        , m_detailsIndexOwned(false)
{
    createIndices(query, field1, field2);
}

KDbRelationship::~KDbRelationship()
{
    if (m_masterIndexOwned)
        delete m_masterIndex;
    if (m_detailsIndexOwned)
        delete m_detailsIndex;
}

void KDbRelationship::createIndices(KDbQuerySchema *query, KDbField *field1, KDbField *field2)
{
    if (!field1 || !field2 || !query) {
        kdbWarning() << "!masterField || !detailsField || !query";
        return;
    }
    if (field1->isQueryAsterisk() || field2->isQueryAsterisk()) {
        kdbWarning() << "relationship's fields cannot be asterisks";
        return;
    }
    if (field1->table() == field2->table()) {
        kdbWarning() << "fields cannot belong to the same table";
        return;
    }
    if (!query->contains(field1->table()) || !query->contains(field2->table())) {
        kdbWarning() << "fields do not belong to this query";
        return;
    }
//! @todo: check more things: -types
//! @todo: find existing global db relationships

    KDbField *masterField = 0, *detailsField = 0;
    bool p1 = field1->isPrimaryKey(), p2 = field2->isPrimaryKey();
    if (p1 && p2) {
        //2 primary keys
        masterField = field1;
        m_masterIndex = masterField->table()->primaryKey();
        detailsField = field2;
        m_detailsIndex = detailsField->table()->primaryKey();
    } else if (!p1 && p2) {
        //foreign + primary: swap
        KDbField *tmp = field1;
        field1 = field2;
        field2 = tmp;
        p1 = !p1;
        p2 = !p2;
    }

    if (p1 && !p2) {
        //primary + foreign
        masterField = field1;
        m_masterIndex = masterField->table()->primaryKey();
        detailsField = field2;
        //create foreign key
//@todo: check if it already exists
        m_detailsIndex = new KDbIndexSchema;
        detailsField->table()->addIndex(m_detailsIndex);
        m_detailsIndexOwned = true;
        const bool ok = m_detailsIndex->addField(detailsField);
        Q_ASSERT(ok);
        m_detailsIndex->setForeignKey(true);
    } else if (!p1 && !p2) {
        masterField = field1;
        m_masterIndex = new KDbIndexSchema;
        masterField->table()->addIndex(m_masterIndex);
        m_masterIndexOwned = true;
        bool ok = m_masterIndex->addField(masterField);
        Q_ASSERT(ok);
        m_masterIndex->setForeignKey(true);

        detailsField = field2;
        m_detailsIndex = new KDbIndexSchema;
        detailsField->table()->addIndex(m_detailsIndex);
        m_detailsIndexOwned = true;
        ok = m_detailsIndex->addField(detailsField);
        Q_ASSERT(ok);
        m_detailsIndex->setForeignKey(true);
    }

    if (!m_masterIndex || !m_detailsIndex)
        return; //failed

    (void)setIndices(m_masterIndex, m_detailsIndex, false);
}

KDbTableSchema* KDbRelationship::masterTable() const
{
    return m_masterIndex ? m_masterIndex->table() : 0;
}

KDbTableSchema* KDbRelationship::detailsTable() const
{
    return m_detailsIndex ? m_detailsIndex->table() : 0;
}

bool KDbRelationship::setIndices(KDbIndexSchema* masterIndex, KDbIndexSchema* detailsIndex)
{
    return setIndices(masterIndex, detailsIndex, true);
}

bool KDbRelationship::setIndices(KDbIndexSchema* masterIndex, KDbIndexSchema* detailsIndex, bool ownedByMaster)
{
    m_masterIndex = 0;
    m_detailsIndex = 0;
    m_pairs.clear();
    if (!masterIndex || !detailsIndex || !masterIndex->table() || !detailsIndex->table()
            || masterIndex->table() == detailsIndex->table() || masterIndex->fieldCount() != detailsIndex->fieldCount())
    {
        return false;
    }
    const KDbField::List* masterIndexFields = masterIndex->fields();
    const KDbField::List* detailsIndexFields = detailsIndex->fields();
    KDbField::ListIterator masterIt(masterIndexFields->constBegin());
    KDbField::ListIterator detailsIt(detailsIndexFields->constBegin());
    for (;masterIt != masterIndexFields->constEnd() && detailsIt != detailsIndexFields->constEnd();
            ++masterIt, ++detailsIt) {
        KDbField *masterField = *masterIt;
        KDbField *detailsField = *detailsIt;
        const KDbField::Type masterType = masterField->type(); // cache: evaluating type of expressions can be expensive
        const KDbField::Type detailsType = detailsField->type();
        if (masterType != detailsType
                && KDbField::isIntegerType(masterType) != KDbField::isIntegerType(detailsType)
                && KDbField::isTextType(masterType) != KDbField::isTextType(detailsType))
        {
            kdbWarning() << "INDEX on" << masterIndex->table()->name()
                << ", INDEX on" << detailsIndex->table()->name() << ": !equal field types:"
                << KDbDriver::defaultSQLTypeName(masterType) << masterField->name() << ","
                << KDbDriver::defaultSQLTypeName(detailsType) << detailsField->name();
            m_pairs.clear();
            return false;
        }
#if 0 //too STRICT!
        if ((masterField->isUnsigned() && !detailsField->isUnsigned())
                || (!masterField->isUnsigned() && detailsField->isUnsigned())) {
            kdbWarning() << "KDbRelationship::setIndices(INDEX on '" << masterIndex->table()->name()
            << "',INDEX on " << detailsIndex->table()->name() << "): !equal signedness of field types: "
            << KDbDriver::defaultSQLTypeName(masterField->type()) << " " << masterField->name() << ", "
            << KDbDriver::defaultSQLTypeName(detailsField->type()) << " " << detailsField->name();
            m_pairs.clear();
            return;
        }
#endif
        m_pairs.append(KDbField::Pair(masterField, detailsField));
    }
    //ok: update information
    if (m_masterIndex) {//detach yourself
        m_masterIndex->detachRelationship(this);
    }
    if (m_detailsIndex) {//detach yourself
        m_detailsIndex->detachRelationship(this);
    }
    m_masterIndex = masterIndex;
    m_detailsIndex = detailsIndex;
    m_masterIndex->attachRelationship(this, ownedByMaster);
    m_detailsIndex->attachRelationship(this, ownedByMaster);
    return true;
}
