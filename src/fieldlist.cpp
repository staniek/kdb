/* This file is part of the KDE project
   Copyright (C) 2003-2005 Jaroslaw Staniek <js@iidea.pl>

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

#include <kexidb/fieldlist.h>
#include <kexidb/object.h>

#include <kdebug.h>

#include <assert.h>

using namespace KexiDB;

FieldList::FieldList(bool owner)
 //reasonable sizes: TODO
 : m_fields_by_name(101, false)
{
	m_fields.setAutoDelete( owner );
	m_fields_by_name.setAutoDelete( false );
	m_autoinc_fields = 0;
}

FieldList::FieldList(const FieldList& fl)
 : m_fields_by_name( fl.m_fields_by_name.size() )
{
	m_fields.setAutoDelete( fl.m_fields.autoDelete() );
	m_fields_by_name.setAutoDelete( false );
	m_autoinc_fields = 0;

	//deep copy for the fields
	for (Field::ListIterator f_it(fl.m_fields); f_it.current(); ++f_it) {
		Field *f = new Field( *f_it.current() );
		f->m_parent = this;
		addField( f );
	}
}

FieldList::~FieldList()
{
	delete m_autoinc_fields;
}

void FieldList::clear()
{
//	m_name = QString::null;
	m_fields.clear();
	m_fields_by_name.clear();
	m_sqlFields = QString::null;
	delete m_autoinc_fields;
	m_autoinc_fields = 0;
}

FieldList& FieldList::insertField(uint index, KexiDB::Field *field)
{
	assert(field);
	if (!field)
		return *this;
	if (index>m_fields.count()) {
		kWarning() << "FieldList::insertField(): index (" << index << ") out of range" << endl;
		return *this;
	}
	if (!m_fields.insert(index, field))
		return *this;
	if (!field->name().isEmpty())
		m_fields_by_name.insert(field->name().toLower(),field);
	m_sqlFields = QString::null;
	return *this;
}

FieldList& FieldList::addField(KexiDB::Field *field)
{
	return insertField(m_fields.count(), field);
}

void FieldList::removeField(KexiDB::Field *field)
{
	assert(field);
	if (!field)
		return;
	m_fields_by_name.remove(field->name());
	m_fields.remove(field);
	m_sqlFields = QString::null;
}

Field* FieldList::field(const QString& name)
{
	return m_fields_by_name[name];
}

QString FieldList::debugString()
{
	QString dbg;
	dbg.reserve(512);
	Field::ListIterator it( m_fields );
	Field *field;
	bool start = true;
	if (!it.current())
		dbg = "<NO FIELDS>";
	for (; (field = it.current())!=0; ++it) {
		if (!start)
			dbg += ",\n";
		else
			start = false;
		dbg += "  ";
		dbg += field->debugString();
	}
	return dbg;
}

void FieldList::debug()
{
	KexiDBDbg << debugString() << endl;
}

#define _ADD_FIELD(fname) \
{ \
	if (fname.isEmpty()) return fl; \
	f = m_fields_by_name[fname]; \
	if (!f) { delete fl; return 0; } \
	fl->addField(f); \
}

FieldList* FieldList::subList(const QString& n1, const QString& n2, 
	const QString& n3, const QString& n4,
	const QString& n5, const QString& n6,
	const QString& n7, const QString& n8,
	const QString& n9, const QString& n10,
	const QString& n11, const QString& n12,
	const QString& n13, const QString& n14,
	const QString& n15, const QString& n16,
	const QString& n17, const QString& n18)
{
	if (n1.isEmpty())
		return 0;
	Field *f;
	FieldList *fl = new FieldList(false);
	_ADD_FIELD(n1);
	_ADD_FIELD(n2);
	_ADD_FIELD(n3);
	_ADD_FIELD(n4);
	_ADD_FIELD(n5);
	_ADD_FIELD(n6);
	_ADD_FIELD(n7);
	_ADD_FIELD(n8);
	_ADD_FIELD(n9);
	_ADD_FIELD(n10);
	_ADD_FIELD(n11);
	_ADD_FIELD(n12);
	_ADD_FIELD(n13);
	_ADD_FIELD(n14);
	_ADD_FIELD(n15);
	_ADD_FIELD(n16);
	_ADD_FIELD(n17);
	_ADD_FIELD(n18);
	return fl;
}

FieldList* FieldList::subList(const QStringList& list)
{
        Field *f;
	FieldList *fl = new FieldList(false);
	for(QStringList::ConstIterator it = list.constBegin(); it != list.constEnd(); ++it) {
		_ADD_FIELD( (*it) );
	}
	return fl;
}

QStringList FieldList::names() const
{
	QStringList r;
//	for (QDictIterator<Field> it(m_fields_by_name);it.current();++it) {
//		r += it.currentKey().toLower();
//	}
	for (Field::ListIterator it(m_fields); it.current(); ++it) {
		r += it.current()->name().toLower();
	}
	return r;
}

QString FieldList::sqlFieldsList(Field::List* list, Driver *driver)
{
	if (!list)
		return QString::null;
	QString result;
	result.reserve(256);
	Field::ListIterator it( *list );
	bool start = true;
	for (; it.current(); ++it) {
		if (!start)
			result += ",";
		else
			start = false;
		result += driver->escapeIdentifier( it.current()->name() );
	}
	return result;
}

QString FieldList::sqlFieldsList(Driver *driver)
{
	if (!m_sqlFields.isEmpty())
		return m_sqlFields;

	m_sqlFields = FieldList::sqlFieldsList( &m_fields, driver );
	return m_sqlFields;
}

Field::List* FieldList::autoIncrementFields()
{
	if (!m_autoinc_fields) {
		m_autoinc_fields = new Field::List();
		Field *f;
		for (Field::ListIterator f_it(m_fields); (f = f_it.current()); ++f_it) {
			if (f->isAutoIncrement()) {
				m_autoinc_fields->append( f_it.current() );
			}
		}
	}
	return m_autoinc_fields;
}
