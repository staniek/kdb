/* This file is part of the KDE project
   Copyright (C) 2006-2012 Jarosław Staniek <staniek@kde.org>

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

#include "KDbAlter.h"
#include "KDb.h"
#include "KDbConnection.h"
#include "kdb_debug.h"

#include <QMap>

#include <stdlib.h>

class KDbAlterTableHandler::Private
{
public:
    Private() {}
    ~Private() {
        qDeleteAll(actions);
    }
    ActionList actions;
//! @todo IMPORTANT: replace QPointer<KDbConnection> conn;
    KDbConnection* conn;
};

//! a global instance used to when returning null is needed
KDbAlterTableHandler::ChangeFieldPropertyAction nullChangeFieldPropertyAction(true);
KDbAlterTableHandler::RemoveFieldAction nullRemoveFieldAction(true);
KDbAlterTableHandler::InsertFieldAction nullInsertFieldAction(true);
KDbAlterTableHandler::MoveFieldPositionAction nullMoveFieldPositionAction(true);

//--------------------------------------------------------

KDbAlterTableHandler::ActionBase::ActionBase(bool null)
        : m_alteringRequirements(0)
        , m_order(-1)
        , m_null(null)
{
}

KDbAlterTableHandler::ActionBase::~ActionBase()
{
}

KDbAlterTableHandler::ChangeFieldPropertyAction& KDbAlterTableHandler::ActionBase::toChangeFieldPropertyAction()
{
    if (dynamic_cast<ChangeFieldPropertyAction*>(this))
        return *dynamic_cast<ChangeFieldPropertyAction*>(this);
    return nullChangeFieldPropertyAction;
}

KDbAlterTableHandler::RemoveFieldAction& KDbAlterTableHandler::ActionBase::toRemoveFieldAction()
{
    if (dynamic_cast<RemoveFieldAction*>(this))
        return *dynamic_cast<RemoveFieldAction*>(this);
    return nullRemoveFieldAction;
}

KDbAlterTableHandler::InsertFieldAction& KDbAlterTableHandler::ActionBase::toInsertFieldAction()
{
    if (dynamic_cast<InsertFieldAction*>(this))
        return *dynamic_cast<InsertFieldAction*>(this);
    return nullInsertFieldAction;
}

KDbAlterTableHandler::MoveFieldPositionAction& KDbAlterTableHandler::ActionBase::toMoveFieldPositionAction()
{
    if (dynamic_cast<MoveFieldPositionAction*>(this))
        return *dynamic_cast<MoveFieldPositionAction*>(this);
    return nullMoveFieldPositionAction;
}

void KDbAlterTableHandler::ActionBase::debug(const DebugOptions& debugOptions)
{
    kdbDebug() << debugString(debugOptions)
    << " (req = " << alteringRequirements() << ")";
}

//--------------------------------------------------------

KDbAlterTableHandler::FieldActionBase::FieldActionBase(const QString& fieldName, int uid)
        : ActionBase()
        , m_fieldUID(uid)
        , m_fieldName(fieldName)
{
}

KDbAlterTableHandler::FieldActionBase::FieldActionBase(bool)
        : ActionBase(true)
        , m_fieldUID(-1)
{
}

KDbAlterTableHandler::FieldActionBase::~FieldActionBase()
{
}

//--------------------------------------------------------

//! @internal
struct KDb_AlterTableHandlerStatic {
    KDb_AlterTableHandlerStatic() {
#define I(name, type) \
    types.insert(QByteArray(name).toLower(), int(KDbAlterTableHandler::type))
#define I2(name, type1, type2) \
    flag = int(KDbAlterTableHandler::type1)|int(KDbAlterTableHandler::type2); \
    if (flag & KDbAlterTableHandler::PhysicalAlteringRequired) \
        flag |= KDbAlterTableHandler::MainSchemaAlteringRequired; \
    types.insert(QByteArray(name).toLower(), flag)

        /* useful links:
          http://dev.mysql.com/doc/refman/5.0/en/create-table.html
        */
        // ExtendedSchemaAlteringRequired is here because when the field is renamed,
        // we need to do the same rename in extended table schema: <field name="...">
        int flag;
        I2("name", PhysicalAlteringRequired, MainSchemaAlteringRequired);
        I2("type", PhysicalAlteringRequired, DataConversionRequired);
        I("caption", MainSchemaAlteringRequired);
        I("description", MainSchemaAlteringRequired);
        I2("unsigned", PhysicalAlteringRequired, DataConversionRequired); // always?
        I2("maxLength", PhysicalAlteringRequired, DataConversionRequired); // always?
        I2("precision", PhysicalAlteringRequired, DataConversionRequired); // always?
        I("defaultWidth", ExtendedSchemaAlteringRequired);
        // defaultValue: depends on backend, for mysql it can only by a constant or now()...
        // -- should we look at KDbDriver here?
#ifdef KDB_UNFINISHED
        I2("defaultValue", PhysicalAlteringRequired, MainSchemaAlteringRequired);
#else
        //! @todo reenable
        I("defaultValue", MainSchemaAlteringRequired);
#endif
        I2("primaryKey", PhysicalAlteringRequired, DataConversionRequired);
        I2("unique", PhysicalAlteringRequired, DataConversionRequired); // we may want to add an Index here
        I2("notNull", PhysicalAlteringRequired, DataConversionRequired); // we may want to add an Index here
        // allowEmpty: only support it just at kexi level? maybe there is a backend that supports this?
        I2("allowEmpty", PhysicalAlteringRequired, MainSchemaAlteringRequired);
        I2("autoIncrement", PhysicalAlteringRequired, DataConversionRequired); // data conversion may be hard here
        I2("indexed", PhysicalAlteringRequired, DataConversionRequired); // we may want to add an Index here

        // easier cases follow...
        I("visibleDecimalPlaces", ExtendedSchemaAlteringRequired);

        //more to come...
#undef I
#undef I2
    }

    QHash<QByteArray, int> types;
};

Q_GLOBAL_STATIC(KDb_AlterTableHandlerStatic, KDb_alteringTypeForProperty)

//! @internal
int KDbAlterTableHandler::alteringTypeForProperty(const QByteArray& propertyName)
{
    const int res = KDb_alteringTypeForProperty->types[propertyName.toLower()];
    if (res == 0) {
        if (KDb::isExtendedTableFieldProperty(propertyName))
            return int(ExtendedSchemaAlteringRequired);
        kdbWarning() << "property" << propertyName << "not found!";
    }
    return res;
}

//---

KDbAlterTableHandler::ChangeFieldPropertyAction::ChangeFieldPropertyAction(
    const QString& fieldName, const QString& propertyName, const QVariant& newValue, int uid)
        : FieldActionBase(fieldName, uid)
        , m_propertyName(propertyName)
        , m_newValue(newValue)
{
}

KDbAlterTableHandler::ChangeFieldPropertyAction::ChangeFieldPropertyAction(bool)
        : FieldActionBase(true)
{
}

KDbAlterTableHandler::ChangeFieldPropertyAction::~ChangeFieldPropertyAction()
{
}

void KDbAlterTableHandler::ChangeFieldPropertyAction::updateAlteringRequirements()
{
    setAlteringRequirements(alteringTypeForProperty(m_propertyName.toLatin1()));
}

QString KDbAlterTableHandler::ChangeFieldPropertyAction::debugString(const DebugOptions& debugOptions)
{
    QString s = QString::fromLatin1("Set \"%1\" property for table field \"%2\" to \"%3\"")
                .arg(m_propertyName).arg(fieldName()).arg(m_newValue.toString());
    if (debugOptions.showUID) {
        s.append(QString::fromLatin1(" (UID=%1)").arg(m_fieldUID));
    }
    return s;
}

static KDbAlterTableHandler::ActionDict* createActionDict(
    KDbAlterTableHandler::ActionDictDict &fieldActions, int forFieldUID)
{
    KDbAlterTableHandler::ActionDict* dict = new KDbAlterTableHandler::ActionDict();
    fieldActions.insert(forFieldUID, dict);
    return dict;
}

static void debugAction(KDbAlterTableHandler::ActionBase *action, int nestingLevel,
                        bool simulate, const QString& prependString = QString(), QString * debugTarget = 0)
{
    Q_UNUSED(simulate);
    Q_UNUSED(nestingLevel);

    QString debugString;
    if (!debugTarget)
        debugString = prependString;
    if (action) {
        KDbAlterTableHandler::ActionBase::DebugOptions debugOptions;
        debugOptions.showUID = debugTarget == 0;
        debugOptions.showFieldDebug = debugTarget != 0;
        debugString += action->debugString(debugOptions);
    } else {
        if (!debugTarget) {
            debugString += QLatin1String("[No action]"); //hmm
        }
    }
    if (debugTarget) {
        if (!debugString.isEmpty()) {
            *debugTarget += debugString + QLatin1Char('\n');
        }
    } else {
        kdbDebug() << debugString;
#ifdef KDB_DEBUG_GUI
        if (simulate)
            KDb::alterTableActionDebugGUI(debugString, nestingLevel);
#endif
    }
}

static void debugActionDict(KDbAlterTableHandler::ActionDict *dict, int fieldUID, bool simulate)
{
    QString fieldName;
    KDbAlterTableHandler::ActionDictConstIterator it(dict->constBegin());
    if (it != dict->constEnd() && dynamic_cast<KDbAlterTableHandler::FieldActionBase*>(it.value())) {
        //retrieve field name from the 1st related action
        fieldName = dynamic_cast<KDbAlterTableHandler::FieldActionBase*>(it.value())->fieldName();
    }
    else {
        fieldName = QLatin1String("??");
    }
    QString dbg(QString::fromLatin1("Action dict for field \"%1\" (%2, UID=%3):")
                        .arg(fieldName).arg(dict->count()).arg(fieldUID));
    kdbDebug() << dbg;
#ifdef KDB_DEBUG_GUI
    if (simulate)
        KDb::alterTableActionDebugGUI(dbg, 1);
#endif
    for (;it != dict->constEnd(); ++it) {
        debugAction(it.value(), 2, simulate);
    }
}

static void debugFieldActions(const KDbAlterTableHandler::ActionDictDict &fieldActions, bool simulate)
{
#ifdef KDB_DEBUG_GUI
    if (simulate)
        KDb::alterTableActionDebugGUI(QLatin1String("** Simplified Field Actions:"));
#endif
    for (KDbAlterTableHandler::ActionDictDictConstIterator it(fieldActions.constBegin()); it != fieldActions.constEnd(); ++it) {
        debugActionDict(it.value(), it.key(), simulate);
    }
}

/*!
 Legend: A,B==fields, P==property, [....]==action, (..,..,..) group of actions, <...> internal operation.
 Case 1. (special)
    when new action=[rename A to B]
    and exists=[rename B to C]
    =>
    remove [rename B to C]
    and set result to new [rename A to C]
    and go to 1b.
 Case 1b. when new action=[rename A to B]
    and actions exist like [set property P to C in field B]
    or like [delete field B]
    or like [move field B]
    =>
    change B to A for all these actions
 Case 2. when new action=[change property in field A] (property != name)
    and exists=[remove A] or exists=[change property in field A]
    =>
    do not add [change property in field A] because it will be removed anyway or the property will change
*/
void KDbAlterTableHandler::ChangeFieldPropertyAction::simplifyActions(ActionDictDict &fieldActions)
{
    ActionDict *actionsLikeThis = fieldActions.value(uid());
    if (m_propertyName == QLatin1String("name")) {
        // Case 1. special: name1 -> name2, i.e. rename action
        QByteArray newName(newValue().toString().toLatin1());
        // try to find rename(newName, otherName) action
        ActionBase *renameActionLikeThis = actionsLikeThis ? actionsLikeThis->value(newName) : 0;
        if (dynamic_cast<ChangeFieldPropertyAction*>(renameActionLikeThis)) {
            // 1. instead of having rename(fieldName(), newValue()) action,
            // let's have rename(fieldName(), otherName) action
            m_newValue = dynamic_cast<ChangeFieldPropertyAction*>(renameActionLikeThis)->m_newValue;
            /*   KDbAlterTableHandler::ChangeFieldPropertyAction* newRenameAction
                    = new KDbAlterTableHandler::ChangeFieldPropertyAction( *this );
                  newRenameAction->m_newValue = dynamic_cast<ChangeFieldPropertyAction*>(renameActionLikeThis)->m_newValue;
                  // (m_order is the same as in newAction)
                  // replace prev. rename action (if any)
                  actionsLikeThis->remove( "name" );
                  ActionDict *adict = fieldActions[ fieldName().toLatin1() ];
                  if (!adict)
                    adict = createActionDict( fieldActions, fieldName() );
                  adict->insert(m_propertyName.toLatin1(), newRenameAction);*/
        } else {
            ActionBase *removeActionForThisField = actionsLikeThis ? actionsLikeThis->value(":remove:") : 0;
            if (removeActionForThisField) {
                //if this field is going to be removed, just change the action's field name
                // and do not add a new action
            } else {
                //just insert a copy of the rename action
                if (!actionsLikeThis)
                    actionsLikeThis = createActionDict(fieldActions, uid());
                KDbAlterTableHandler::ChangeFieldPropertyAction* newRenameAction
                    = new KDbAlterTableHandler::ChangeFieldPropertyAction(*this);
                kdbDebug() << "insert into" << fieldName() << "dict:" << newRenameAction->debugString();
                actionsLikeThis->insert(m_propertyName.toLatin1(), newRenameAction);
                return;
            }
        }
        if (actionsLikeThis) {
            // Case 1b. change "field name" information to fieldName() in any action that
            //    is related to newName
            //    e.g. if there is setCaption("B", "captionA") action after rename("A","B"),
            //    replace setCaption action with setCaption("A", "captionA")
            foreach(ActionBase* action, *actionsLikeThis) {
                dynamic_cast<FieldActionBase*>(action)->setFieldName(fieldName());
            }
        }
        return;
    }
    ActionBase *removeActionForThisField = actionsLikeThis ? actionsLikeThis->value(":remove:") : 0;
    if (removeActionForThisField) {
        //if this field is going to be removed, do not add a new action
        return;
    }
    // Case 2. other cases: just give up with adding this "intermediate" action
    // so, e.g. [ setCaption(A, "captionA"), setCaption(A, "captionB") ]
    //  becomes: [ setCaption(A, "captionB") ]
    // because adding this action does nothing
    ActionDict *nextActionsLikeThis = fieldActions.value(uid());
    if (!nextActionsLikeThis || !nextActionsLikeThis->value(m_propertyName.toLatin1())) {
        //no such action, add this
        KDbAlterTableHandler::ChangeFieldPropertyAction* newAction
            = new KDbAlterTableHandler::ChangeFieldPropertyAction(*this);
        if (!nextActionsLikeThis)
            nextActionsLikeThis = createActionDict(fieldActions, uid());
        nextActionsLikeThis->insert(m_propertyName.toLatin1(), newAction);
    }
}

bool KDbAlterTableHandler::ChangeFieldPropertyAction::shouldBeRemoved(ActionDictDict &fieldActions)
{
    Q_UNUSED(fieldActions);
    return 0 == fieldName().compare(m_newValue.toString(), Qt::CaseInsensitive);
}

tristate KDbAlterTableHandler::ChangeFieldPropertyAction::updateTableSchema(KDbTableSchema* table, KDbField* field,
        QHash<QString, QString>* fieldHash)
{
    //1. Simpler cases first: changes that do not affect table schema at all
    // "caption", "description", "defaultWidth", "visibleDecimalPlaces"
    if (SchemaAlteringRequired & alteringTypeForProperty(m_propertyName.toLatin1())) {
        bool result = KDb::setFieldProperty(field, m_propertyName.toLatin1(), newValue());
        return result;
    }

    if (m_propertyName == QLatin1String("name")) {
        if (fieldHash->value(field->name()) == field->name())
            fieldHash->remove(field->name());
        fieldHash->insert(newValue().toString(), field->name());
        table->renameField(field, newValue().toString());
        return true;
    }
    return cancelled;
}

/*! Many of the properties must be applied using a separate algorithm.
*/
tristate KDbAlterTableHandler::ChangeFieldPropertyAction::execute(KDbConnection* conn, KDbTableSchema* table)
{
    Q_UNUSED(conn);
    KDbField *field = table->field(fieldName());
    if (!field) {
        //! @todo errmsg
        return false;
    }
    bool result;
    //1. Simpler cases first: changes that do not affect table schema at all
    // "caption", "description", "defaultWidth", "visibleDecimalPlaces"
    if (SchemaAlteringRequired & alteringTypeForProperty(m_propertyName.toLatin1())) {
        result = KDb::setFieldProperty(field, m_propertyName.toLatin1(), newValue());
        return result;
    }

//! @todo
#if 1
    return true;
#else
    //2. Harder cases, that often require special care
    if (m_propertyName == QLatin1String("name")) {
        /*mysql:
         A. Get real field type (it's safer):
            let <TYPE> be the 2nd "Type" column from result of "DESCRIBE tablename oldfieldname"
          ( http://dev.mysql.com/doc/refman/5.0/en/describe.html )
         B. Run "ALTER TABLE tablename CHANGE oldfieldname newfieldname <TYPE>";
          ( http://dev.mysql.com/doc/refman/5.0/en/alter-table.html )
        */
    }
    if (m_propertyName == QLatin1String("type")) {
        /*mysql:
         A. Like A. for "name" property above
         B. Construct <TYPE> string, eg. "varchar(50)" using the driver
         C. Like B. for "name" property above
         (mysql then truncate the values for changes like varchar -> integer,
         and properly convert the values for changes like integer -> varchar)
        */
        //! @todo more cases to check
    }
    if (m_propertyName == QLatin1String("maxLength")) {
        //! @todo use "select max( length(o_name) ) from kexi__objects"

    }
    if (m_propertyName == QLatin1String("primaryKey")) {
//! @todo
    }

    /*
         "name", "unsigned", "precision",
         "defaultValue", "primaryKey", "unique", "notNull", "allowEmpty",
         "autoIncrement", "indexed",


      bool result = KDb::setFieldProperty(*field, m_propertyName.toLatin1(), newValue());
    */
    return result;
#endif
}

//--------------------------------------------------------

KDbAlterTableHandler::RemoveFieldAction::RemoveFieldAction(const QString& fieldName, int uid)
        : FieldActionBase(fieldName, uid)
{
}

KDbAlterTableHandler::RemoveFieldAction::RemoveFieldAction(bool)
        : FieldActionBase(true)
{
}

KDbAlterTableHandler::RemoveFieldAction::~RemoveFieldAction()
{
}

void KDbAlterTableHandler::RemoveFieldAction::updateAlteringRequirements()
{
//! @todo sometimes add DataConversionRequired (e.g. when relationships require removing orphaned records) ?

    setAlteringRequirements(PhysicalAlteringRequired);
    //! @todo
}

QString KDbAlterTableHandler::RemoveFieldAction::debugString(const DebugOptions& debugOptions)
{
    QString s = QString::fromLatin1("Remove table field \"%1\"").arg(fieldName());
    if (debugOptions.showUID) {
        s.append(QString::fromLatin1(" (UID=%1)").arg(uid()));
    }
    return s;
}

/*!
 Legend: A,B==objects, P==property, [....]==action, (..,..,..) group of actions, <...> internal operation.
 Preconditions: we assume there cannot be such case encountered: ([remove A], [do something related on A])
  (except for [remove A], [insert A])
 General Case: it's safe to always insert a [remove A] action.
*/
void KDbAlterTableHandler::RemoveFieldAction::simplifyActions(ActionDictDict &fieldActions)
{
    //! @todo not checked
    KDbAlterTableHandler::RemoveFieldAction* newAction
        = new KDbAlterTableHandler::RemoveFieldAction(*this);
    ActionDict *actionsLikeThis = fieldActions.value(uid());
    if (!actionsLikeThis)
        actionsLikeThis = createActionDict(fieldActions, uid());
    actionsLikeThis->insert(":remove:", newAction);   //special
}

tristate KDbAlterTableHandler::RemoveFieldAction::updateTableSchema(KDbTableSchema* table, KDbField* field,
        QHash<QString, QString>* fieldHash)
{
    fieldHash->remove(field->name());
    table->removeField(field);
    return true;
}

tristate KDbAlterTableHandler::RemoveFieldAction::execute(KDbConnection* conn, KDbTableSchema* table)
{
    Q_UNUSED(conn);
    Q_UNUSED(table);
    //! @todo
    return true;
}

//--------------------------------------------------------

KDbAlterTableHandler::InsertFieldAction::InsertFieldAction(int fieldIndex, KDbField *field, int uid)
        : FieldActionBase(field->name(), uid)
        , m_index(fieldIndex)
        , m_field(0)
{
    Q_ASSERT(field);
    setField(field);
}

KDbAlterTableHandler::InsertFieldAction::InsertFieldAction(const InsertFieldAction& action)
        : FieldActionBase(action) //action.fieldName(), action.uid())
        , m_index(action.index())
{
    m_field = new KDbField(*action.field());
}

KDbAlterTableHandler::InsertFieldAction::InsertFieldAction(bool)
        : FieldActionBase(true)
        , m_index(0)
        , m_field(0)
{
}

KDbAlterTableHandler::InsertFieldAction::~InsertFieldAction()
{
    delete m_field;
}

void KDbAlterTableHandler::InsertFieldAction::setField(KDbField* field)
{
    if (m_field)
        delete m_field;
    m_field = field;
    setFieldName(m_field ? m_field->name() : QString());
}

void KDbAlterTableHandler::InsertFieldAction::updateAlteringRequirements()
{
//! @todo sometimes add DataConversionRequired (e.g. when relationships require removing orphaned records) ?

    setAlteringRequirements(PhysicalAlteringRequired);
    //! @todo
}

QString KDbAlterTableHandler::InsertFieldAction::debugString(const DebugOptions& debugOptions)
{
    QString s = QString::fromLatin1("Insert table field \"%1\" at position %2")
                .arg(m_field->name()).arg(m_index);
    if (debugOptions.showUID) {
        s.append(QString::fromLatin1(" (UID=%1)").arg(m_fieldUID));
    }
    if (debugOptions.showFieldDebug) {
        s.append(QString::fromLatin1(" (%1)").arg(KDbUtils::debugString<KDbField>(*m_field)));
    }
    return s;
}

/*!
 Legend: A,B==fields, P==property, [....]==action, (..,..,..) group of actions, <...> internal operation.


 Case 1: there are "change property" actions after the Insert action.
  -> change the properties in the Insert action itself and remove the "change property" actions.
 Examples:
   [Insert A] && [rename A to B] => [Insert B]
   [Insert A] && [change property P in field A] => [Insert A with P altered]
 Comment: we need to do this reduction because otherwise we'd need to do psyhical altering
  right after [Insert A] if [rename A to B] follows.
*/
void KDbAlterTableHandler::InsertFieldAction::simplifyActions(ActionDictDict &fieldActions)
{
    // Try to find actions related to this action
    ActionDict *actionsForThisField = fieldActions.value(uid());

    ActionBase *removeActionForThisField = actionsForThisField ? actionsForThisField->value(":remove:") : 0;
    if (removeActionForThisField) {
        //if this field is going to be removed, do not add a new action
        //and remove the "Remove" action
        actionsForThisField->remove(":remove:");
        return;
    }
    if (actionsForThisField) {
        //collect property values that have to be changed in this field
        QMap<QByteArray, QVariant> values;
        ActionDict *newActionsForThisField = new ActionDict(); // this will replace actionsForThisField after the loop
        QSet<ActionBase*> actionsToDelete; // used to collect actions taht we soon delete but cannot delete in the loop below
        for (ActionDictConstIterator it(actionsForThisField->constBegin()); it != actionsForThisField->constEnd();++it) {
            ChangeFieldPropertyAction* changePropertyAction = dynamic_cast<ChangeFieldPropertyAction*>(it.value());
            if (changePropertyAction) {
                //if this field is going to be renamed, also update fieldName()
                if (changePropertyAction->propertyName() == QLatin1String("name")) {
                    setFieldName(changePropertyAction->newValue().toString());
                }
                values.insert(changePropertyAction->propertyName().toLatin1(), changePropertyAction->newValue());
                //the subsequent "change property" action is no longer needed
                actionsToDelete.insert(it.value());
            } else {
                //keep
                newActionsForThisField->insert(it.key(), it.value());
            }
        }
        qDeleteAll(actionsToDelete);
        actionsForThisField->setAutoDelete(false);
        delete actionsForThisField;
        actionsForThisField = newActionsForThisField;
        fieldActions.take(uid());
        fieldActions.insert(uid(), actionsForThisField);
        if (!values.isEmpty()) {
            //update field, so it will be created as one step
            KDbField *f = new KDbField(*field());
            if (KDb::setFieldProperties(f, values)) {
                setField(f);
                kdbDebug() << field();
#ifdef KDB_DEBUG_GUI
                KDb::alterTableActionDebugGUI(
                    QLatin1String("** Property-set actions moved to field definition itself:\n")
                        + KDbUtils::debugString<KDbField>(*field()), 0);
#endif
            } else {
#ifdef KDB_DEBUG_GUI
                KDb::alterTableActionDebugGUI(
                    QLatin1String("** Failed to set properties for field ") + KDbUtils::debugString<KDbField>(*field()), 0);
#endif
                kdbWarning() << "setFieldProperties() failed!";
                delete f;
            }
        }
    }
    //ok, insert this action
    //! @todo not checked
    KDbAlterTableHandler::InsertFieldAction* newAction
        = new KDbAlterTableHandler::InsertFieldAction(*this);
    if (!actionsForThisField)
        actionsForThisField = createActionDict(fieldActions, uid());
    actionsForThisField->insert(":insert:", newAction);   //special
}

tristate KDbAlterTableHandler::InsertFieldAction::updateTableSchema(KDbTableSchema* table, KDbField* field,
        QHash<QString, QString>* fieldMap)
{
    //in most cases we won't add the field to fieldMap
    Q_UNUSED(field);
//! @todo add it only when there should be fixed value (e.g. default) set for this new field...
    fieldMap->remove(this->field()->name());
    table->insertField(index(), new KDbField(*this->field()));
    return true;
}

tristate KDbAlterTableHandler::InsertFieldAction::execute(KDbConnection* conn, KDbTableSchema* table)
{
    Q_UNUSED(conn);
    Q_UNUSED(table);
    //! @todo
    return true;
}

//--------------------------------------------------------

KDbAlterTableHandler::MoveFieldPositionAction::MoveFieldPositionAction(
    int fieldIndex, const QString& fieldName, int uid)
        : FieldActionBase(fieldName, uid)
        , m_index(fieldIndex)
{
}

KDbAlterTableHandler::MoveFieldPositionAction::MoveFieldPositionAction(bool)
        : FieldActionBase(true)
        , m_index(-1)
{
}

KDbAlterTableHandler::MoveFieldPositionAction::~MoveFieldPositionAction()
{
}

void KDbAlterTableHandler::MoveFieldPositionAction::updateAlteringRequirements()
{
    setAlteringRequirements(MainSchemaAlteringRequired);
    //! @todo
}

QString KDbAlterTableHandler::MoveFieldPositionAction::debugString(const DebugOptions& debugOptions)
{
    QString s = QString::fromLatin1("Move table field \"%1\" to position %2")
                .arg(fieldName()).arg(m_index);
    if (debugOptions.showUID) {
        s.append(QString::fromLatin1(" (UID=%1)").arg(uid()));
    }
    return s;
}

void KDbAlterTableHandler::MoveFieldPositionAction::simplifyActions(ActionDictDict &fieldActions)
{
    Q_UNUSED(fieldActions);
    //! @todo
}

tristate KDbAlterTableHandler::MoveFieldPositionAction::execute(KDbConnection* conn, KDbTableSchema* table)
{
    Q_UNUSED(conn);
    Q_UNUSED(table);
    //! @todo
    return true;
}

//--------------------------------------------------------

KDbAlterTableHandler::KDbAlterTableHandler(KDbConnection* conn)
        : d(new Private())
{
    d->conn = conn;
}

KDbAlterTableHandler::~KDbAlterTableHandler()
{
    delete d;
}

void KDbAlterTableHandler::addAction(ActionBase* action)
{
    d->actions.append(action);
}

KDbAlterTableHandler& KDbAlterTableHandler::operator<< (ActionBase* action)
{
    d->actions.append(action);
    return *this;
}

const KDbAlterTableHandler::ActionList& KDbAlterTableHandler::actions() const
{
    return d->actions;
}

void KDbAlterTableHandler::removeAction(int index)
{
    d->actions.removeAt(index);
}

void KDbAlterTableHandler::clear()
{
    d->actions.clear();
}

void KDbAlterTableHandler::setActions(const ActionList& actions)
{
    qDeleteAll(d->actions);
    d->actions = actions;
}

void KDbAlterTableHandler::debug()
{
    kdbDebug() << "KDbAlterTableHandler's actions:";
    foreach(ActionBase* action, d->actions) {
        action->debug();
    }
}

KDbTableSchema* KDbAlterTableHandler::execute(const QString& tableName, ExecutionArguments* args)
{
    args->result = false;
    if (!d->conn) {
//! @todo err msg?
        return 0;
    }
    if (d->conn->options()->isReadOnly()) {
//! @todo err msg?
        return 0;
    }
    if (!d->conn->isDatabaseUsed()) {
//! @todo err msg?
        return 0;
    }
    KDbTableSchema *oldTable = d->conn->tableSchema(tableName);
    if (!oldTable) {
//! @todo err msg?
        return 0;
    }

    if (!args->debugString)
        debug();

    // Find a sum of requirements...
    int allActionsCount = 0;
    foreach(ActionBase* action, d->actions) {
        action->updateAlteringRequirements();
        action->m_order = allActionsCount++;
    }

    /* Simplify actions list if possible and check for errors

    How to do it?
    - track property changes/deletions in reversed order
    - reduce intermediate actions

    Trivial example 1:
     *action1: "rename field a to b"
     *action2: "rename field b to c"
     *action3: "rename field c to d"

     After reduction:
     *action1: "rename field a to d"
     Summing up: we have tracked what happens to field curently named "d"
     and eventually discovered that it was originally named "a".

    Trivial example 2:
     *action1: "rename field a to b"
     *action2: "rename field b to c"
     *action3: "remove field b"
     After reduction:
     *action3: "remove field b"
     Summing up: we have noticed that field "b" has beed eventually removed
     so we needed to find all actions related to this field and remove them.
     This is good optimization, as some of the eventually removed actions would
     be difficult to perform and/or costly, what would be a waste of resources
     and a source of unwanted questions sent to the user.
    */



    // Fields-related actions.
    ActionDictDict fieldActions;
    ActionBase* action;
    for (int i = d->actions.count() - 1; i >= 0; i--) {
        d->actions[i]->simplifyActions(fieldActions);
    }

    if (!args->debugString)
        debugFieldActions(fieldActions, args->simulate);

    // Prepare actions for execution ----
    // - Sort actions by order
    ActionsVector actionsVector(allActionsCount);
    int currentActionsCount = 0; //some actions may be removed
    args->requirements = 0;
    QSet<QString> fieldsWithChangedMainSchema; // Used to collect fields with changed main schema.
    // This will be used when recreateTable is false to update kexi__fields
    for (ActionDictDictConstIterator it(fieldActions.constBegin()); it != fieldActions.constEnd(); ++it) {
        for (KDbAlterTableHandler::ActionDictConstIterator it2(it.value()->constBegin());
             it2 != it.value()->constEnd(); ++it2, currentActionsCount++)
        {
            if (it2.value()->shouldBeRemoved(fieldActions))
                continue;
            actionsVector[ it2.value()->m_order ] = it2.value();
            // a sum of requirements...
            const int r = it2.value()->alteringRequirements();
            args->requirements |= r;
            if (r & MainSchemaAlteringRequired && dynamic_cast<ChangeFieldPropertyAction*>(it2.value())) {
                // Remember, this will be used when recreateTable is false to update kexi__fields, below.
                fieldsWithChangedMainSchema.insert(
                    dynamic_cast<ChangeFieldPropertyAction*>(it2.value())->fieldName());
            }
        }
    }
    // - Debug
    QString dbg = QString::fromLatin1("** Overall altering requirements: %1").arg(args->requirements);
    kdbDebug() << dbg;

    if (args->onlyComputeRequirements) {
        args->result = true;
        return 0;
    }

    const bool recreateTable = (args->requirements & PhysicalAlteringRequired);

#ifdef KDB_DEBUG_GUI
    if (args->simulate)
        KDb::alterTableActionDebugGUI(dbg, 0);
#endif
    dbg = QString::fromLatin1("** Ordered, simplified actions (%1, was %2):")
            .arg(currentActionsCount).arg(allActionsCount);
    kdbDebug() << dbg;
#ifdef KDB_DEBUG_GUI
    if (args->simulate)
        KDb::alterTableActionDebugGUI(dbg, 0);
#endif
    for (int i = 0; i < allActionsCount; i++) {
        debugAction(actionsVector.at(i), 1, args->simulate,
                    QString::fromLatin1("%1: ").arg(i + 1), args->debugString);
    }

    if (args->requirements == 0) {//nothing to do
        args->result = true;
        return oldTable;
    }
    if (args->simulate) {//do not execute
        args->result = true;
        return oldTable;
    }
//! @todo transaction!

    // Create a new KDbTableSchema
    KDbTableSchema *newTable = recreateTable ? new KDbTableSchema(*oldTable, false/*!copy id*/) : oldTable;
    // find nonexisting temp name for new table schema
    if (recreateTable) {
        QString tempDestTableName;
        while (true) {
            tempDestTableName = QString::fromLatin1("%1_temp%2%3")
                .arg(newTable->name()).arg(QString::number(rand(), 16)).arg(QString::number(rand(), 16));
            if (!d->conn->tableSchema(tempDestTableName))
                break;
        }
        newTable->setName(tempDestTableName);
    }
    kdbDebug() << *oldTable;
    if (recreateTable && !args->debugString) {
        kdbDebug() << *newTable;
    }

    // Update table schema in memory ----
    int lastUID = -1;
    KDbField *currentField = 0;
    QHash<QString, QString> fieldHash; // a map from new value to old value
    foreach(KDbField* f, *newTable->fields()) {
        fieldHash.insert(f->name(), f->name());
    }
    for (int i = 0; i < allActionsCount; i++) {
        action = actionsVector.at(i);
        if (!action)
            continue;
        //remember the current KDbField object because soon we may be unable to find it by name:
        FieldActionBase *fieldAction = dynamic_cast<FieldActionBase*>(action);
        if (!fieldAction) {
            currentField = 0;
        } else {
            if (lastUID != fieldAction->uid()) {
                currentField = newTable->field(fieldAction->fieldName());
                lastUID = currentField ? fieldAction->uid() : -1;
            }
            InsertFieldAction *insertFieldAction = dynamic_cast<InsertFieldAction*>(action);
            if (insertFieldAction && insertFieldAction->index() > newTable->fieldCount()) {
                //update index: there can be empty rows
                insertFieldAction->setIndex(newTable->fieldCount());
            }
        }
        args->result = action->updateTableSchema(newTable, currentField, &fieldHash);
        if (args->result != true) {
            if (recreateTable)
                delete newTable;
            return 0;
        }
    }

    if (recreateTable) {
        // Create the destination table with temporary name
        if (!d->conn->createTable(newTable, false)) {
            m_result = d->conn->result();
            delete newTable;
            args->result = false;
            return 0;
        }
    }

#if 0
    //! @todo
    // Execute actions ----
    for (int i = 0; i < allActionsCount; i++) {
        action = actionsVector.at(i);
        if (!action)
            continue;
        args.result = action->execute(*d->conn, *newTable);
        if (!args.result || ~args.result) {
//! @todo delete newTable...
            args.result = false;
            return 0;
        }
    }
#endif

    // update extended table schema after executing the actions
    if (!d->conn->storeExtendedTableSchemaData(newTable)) {
//! @todo better errmsg?
        m_result = d->conn->result();
//! @todo delete newTable...
        args->result = false;
        return 0;
    }

    if (recreateTable) {
        // Copy the data:
        // Build "INSERT INTO ... SELECT FROM ..." SQL statement
        // The order is based on the order of the source table fields.
        // Notes:
        // -Some source fields can be skipped in case when there are deleted fields.
        // -Some destination fields can be skipped in case when there
        //  are new empty fields without fixed/default value.
        KDbEscapedString sql = KDbEscapedString("INSERT INTO %1 (").arg(d->conn->escapeIdentifier(newTable->name()));
        //insert list of dest. fields
        bool first = true;
        KDbEscapedString sourceFields;
        foreach(KDbField* f, *newTable->fields()) {
            QString renamedFieldName(fieldHash.value(f->name()));
            KDbEscapedString sourceSQLString;
            const KDbField::Type type = f->type(); // cache: evaluating type of expressions can be expensive
            if (!renamedFieldName.isEmpty()) {
                //this field should be renamed
                sourceSQLString = KDbEscapedString(d->conn->escapeIdentifier(renamedFieldName));
            } else if (!f->defaultValue().isNull()) {
                //this field has a default value defined
//! @todo support expressions (eg. TODAY()) as a default value
//! @todo this field can be notNull or notEmpty - check whether the default is ok
//!       (or do this checking also in the Table Designer?)
                sourceSQLString = d->conn->driver()->valueToSQL(type, f->defaultValue());
            } else if (f->isNotNull()) {
                //this field cannot be null
                sourceSQLString = d->conn->driver()->valueToSQL(
                                      type, KDb::emptyValueForFieldType(type));
            } else if (f->isNotEmpty()) {
                //this field cannot be empty - use any nonempty value..., e.g. " " for text or 0 for number
                sourceSQLString = d->conn->driver()->valueToSQL(
                                      type, KDb::notEmptyValueForFieldType(type));
            }
//! @todo support unique, validatationRule, unsigned flags...
//! @todo check for foreignKey values...

            if (!sourceSQLString.isEmpty()) {
                if (first) {
                    first = false;
                } else {
                    sql.append(", ");
                    sourceFields.append(", ");
                }
                sql += d->conn->escapeIdentifier(f->name());
                sourceFields.append(sourceSQLString);
            }
        }
        sql += (") SELECT " + sourceFields + " FROM " + oldTable->name());
        kdbDebug() << " ** " << sql;
        if (!d->conn->executeSQL(sql)) {
            m_result = d->conn->result();
//! @todo delete newTable...
            args->result = false;
            return 0;
        }

        const QString oldTableName = oldTable->name();
        /*  args.result = d->conn->dropTable( oldTable );
            if (!args.result || ~args.result) {
              setError(d->conn);
        //! @todo delete newTable...
              return 0;
            }
            oldTable = 0;*/

        // Replace the old table with the new one (oldTable will be destroyed)
        if (!d->conn->alterTableName(newTable, oldTableName, true /*replace*/)) {
            m_result = d->conn->result();
//! @todo delete newTable...
            args->result = false;
            return 0;
        }
        oldTable = 0;
    }

    if (!recreateTable) {
        if ((MainSchemaAlteringRequired & args->requirements) && !fieldsWithChangedMainSchema.isEmpty()) {
            //update main schema (kexi__fields) for changed fields
            foreach(const QString& changeFieldPropertyActionName, fieldsWithChangedMainSchema) {
                KDbField *f = newTable->field(changeFieldPropertyActionName);
                if (f) {
                    if (!d->conn->storeMainFieldSchema(f)) {
                        m_result = d->conn->result();
                        //! @todo delete newTable...
                        args->result = false;
                        return 0;
                    }
                }
            }
        }
    }
    args->result = true;
    return newTable;
}

/*KDbTableSchema* KDbAlterTableHandler::execute(const QString& tableName, tristate &result, bool simulate)
{
  return executeInternal( tableName, result, simulate, 0 );
}

tristate KDbAlterTableHandler::simulateExecution(const QString& tableName, QString& debugString)
{
  tristate result;
  (void)executeInternal( tableName, result, true//simulate
  , &debugString );
  return result;
}
*/
