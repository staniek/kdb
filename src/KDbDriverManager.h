/* This file is part of the KDE project
   Copyright (C) 2003 Daniel Molkentin <molkentin@kde.org>
   Copyright (C) 2003 Joseph Wenninger <jowenn@kde.org>
   Copyright (C) 2003-2015 Jarosław Staniek <staniek@kde.org>
   Copyright (C) 2012 Dimitrios T. Tanis <dimitrios.tanis@kdemail.net>

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

#ifndef KDB_DRIVER_MANAGER_H
#define KDB_DRIVER_MANAGER_H

#include <QString>

#include "kdb_export.h"

class KDbResult;
class KDbResultable;
class KDbDriver;
class KDbDriverMetaData;

//! A driver manager for finding and loading driver plugins.
class KDB_EXPORT KDbDriverManager
{
public:
    KDbDriverManager();
    virtual ~KDbDriverManager();

    //! @return result of the recent operation.
    KDbResult result() const;

    //! @return KDbResultable object for the recent operation.
    //! It adds serverResultName() in addition to the result().
    KDbResultable resultable() const;

    /*! @return information (metadata) about driver with ID @a id.
      The lookup is case insensitive.
      0 is returned if the metadata has not been found.
      On error status can be obtained using result(). */
    const KDbDriverMetaData* driverMetaData(const QString &id);

    /*! Tries to load db driver with ID @a id.
      The lookup is case insensitive.
      @return driver object or 0 on error.
      On error status can be obtained using result(). */
    KDbDriver* driver(const QString& id);

    /*! returns list of available drivers IDs.
      That drivers can be loaded by first use of driver() method. */
    QStringList driverIds();

    /*! @return list of driver IDs for @a mimeType mime type.
     Empty list is returned if no driver has been found.
     Works only with drivers of file-based databases such as SQLite.
     The lookup is case insensitive. */
    QStringList driverIdsForMimeType(const QString& mimeType);

    /*! @return HTML-formatted message about possible problems encountered.
     It can be displayed in a 'details' section of a GUI message if an error encountered.
     Currently the message contains a list of incompatible db drivers.
     Can be used in code that finds driver depending on file format. */
//! @todo make it just QStringList
    QString possibleProblemsMessage() const;

    /*! @return true if there is at least one server-based database driver installed. */
    bool hasDatabaseServerDrivers();
};

#endif
