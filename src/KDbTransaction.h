/* This file is part of the KDE project
   Copyright (C) 2003 Jarosław Staniek <staniek@kde.org>

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

#ifndef KDB_TRANSACTION_H
#define KDB_TRANSACTION_H

#include <QObject>
#include "kdb_export.h"

class KDbConnection;

/*! Internal prototype for storing KDbTransaction handles for KDbTransaction object.
 Only for driver developers: reimplement this class for driver that
 support KDbTransaction handles.
*/
class KDB_EXPORT KDbTransactionData
{
public:
    explicit KDbTransactionData(KDbConnection *conn);
    ~KDbTransactionData();

    //helper for debugging
    static int globalcount;
    //helper for debugging
    static int globalCount();

    KDbConnection *m_conn;
    bool m_active;
    uint refcount;
};

//! This class encapsulates KDbTransaction handle.
/*! KDbTransaction handle is sql driver-dependent,
  but outside KDbTransaction is visible as universal container
  for any handler implementation.

  KDbTransaction object is value-based, internal data (handle) structure,
  reference-counted.
*/
class KDB_EXPORT KDbTransaction : public QObject
{
public:
/*! Constructs uninitialised (null) transaction.
     Only in Conenction code it can be initialised */
    KDbTransaction();

    //! Copy ctor.
    KDbTransaction(const KDbTransaction& trans);

    virtual ~KDbTransaction();

    KDbTransaction& operator=(const KDbTransaction& trans);

    bool operator==(const KDbTransaction& trans) const;

    KDbConnection* connection() const;

    /*! @return true if transaction is avtive (ie. started)
     Returns false also if transaction is uninitialised (null). */
    bool active() const;

    /*! @return true if transaction is uinitialised (null). */
    bool isNull() const;

    //helper for debugging
    static int globalCount();
    static int globalcount;

protected:
    KDbTransactionData *m_data;

    friend class KDbConnection;
};

//! Helper class for using inside methods for given connection.
/*! It can be used in two ways:
  - start new transaction in constructor and rollback on destruction (1st constructor),
  - use already started transaction and rollback on destruction (2nd constructor).
  In any case, if transaction is committed or rolled back outside this KDbTransactionGuard
  object in the meantime, nothing happens on KDbTransactionGuard destruction.
  <code>
  Example usage:
  void myclas::my_method()
  {
    KDbTransaction *transaction = connection->beginTransaction();
    KDbTransactionGuard tg(transaction);
    ...some code that operates inside started transaction...
    if (something)
      return //after return from this code block: tg will call
               //connection->rollbackTransaction() automatically
    if (something_else)
      transaction->commit();
    //for now tg won't do anything because transaction does not exist
  }
  </code>
*/
class KDB_EXPORT KDbTransactionGuard
{
public:
    /*! Constructor #1: Starts new transaction constructor for @a connection.
     Started KDbTransaction handle is available via transaction().*/
    explicit KDbTransactionGuard(KDbConnection *conn);

    /*! Constructor #2: Uses already started transaction. */
    explicit KDbTransactionGuard(const KDbTransaction& trans);

    /*! Constructor #3: Creates KDbTransactionGuard without transaction assigned.
     setTransaction() can be used later to do so. */
    KDbTransactionGuard();

    /*! Rollbacks not committed transaction. */
    ~KDbTransactionGuard();

    /*! Assigns transaction @a trans to this guard.
     Previously assigned transaction will be unassigned from this guard. */
    void setTransaction(const KDbTransaction& trans) {
        m_trans = trans;
    }

    /*! Comits the guarded transaction.
     It is convenient shortcut to connection->commitTransaction(this->transaction()) */
    bool commit();

    /*! Makes guarded transaction not guarded, so nothing will be performed on guard's desctruction. */
    void doNothing();

    /*! KDbTransaction that are controlled by this guard. */
    const KDbTransaction transaction() const {
        return m_trans;
    }

protected:
    KDbTransaction m_trans;
    bool m_doNothing;
};

#endif