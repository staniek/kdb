/* This file is part of the KDE project
   Copyright (C) 2006 Jarosław Staniek <staniek@kde.org>

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

#include <Predicate/Global.h>
#include "SqliteVacuum.h"

//#include <kstandarddirs.h>
#include <QtDebug>
#include <QMessageBox>
#include <QProgressBar>
#include <QProgressDialog>
#include <QFileInfo>
#include <QDir>
#include <QApplication>
#include <Q3Process>
#include <QCursor>
#include <QLocale>

#ifdef Q_WS_WIN
#include <windows.h>
void usleep(unsigned int usec)
{
	Sleep(usec/1000);
}
#else
#include <unistd.h>
#endif

SQLiteVacuum::SQLiteVacuum(const QString& filePath)
        : m_filePath(filePath)
{
    m_process = 0;
    m_percent = 0;
    m_dlg = 0;
    m_result = true;
}

SQLiteVacuum::~SQLiteVacuum()
{
    delete m_process;
    if (m_dlg)
        m_dlg->close();
    delete m_dlg;
}

tristate SQLiteVacuum::run()
{
//TODO    const QString ksqlite_app = KStandardDirs::findExe("ksqlite");
    const QString ksqlite_app = "ksqlite";
    if (ksqlite_app.isEmpty()) {
        m_result = false;
        return m_result;
    }
    QFileInfo fi(m_filePath);
    if (!fi.isReadable()) {
        PreDrvWarn << "SQLiteVacuum::run(): No such file" << m_filePath;
        return false;
    }
    const uint origSize = fi.size();

    QStringList args;
    args << ksqlite_app << "-verbose-vacuum" << m_filePath << "vacuum";
    m_process = new Q3Process(args, this, "process");
    m_process->setWorkingDirectory(QFileInfo(m_filePath).absoluteDir());
    connect(m_process, SIGNAL(readyReadStdout()), this, SLOT(readFromStdout()));
    connect(m_process, SIGNAL(processExited()), this, SLOT(processExited()));
    if (!m_process->start()) {
        m_result = false;
        return m_result;
    }
    m_dlg = new QProgressDialog(0);
    m_dlg->setWindowTitle(QObject::tr("Compacting database"));
    m_dlg->setLabelText(
        "<qt>" + tr("Compacting database \"%1\"...")
            .arg("<nobr>" + QDir::convertSeparators(QFileInfo(m_filePath).fileName()) + "</nobr>")
    );
    m_dlg->adjustSize();
    m_dlg->resize(300, m_dlg->height());
    connect(m_dlg, SIGNAL(cancelClicked()), this, SLOT(cancelClicked()));
    m_dlg->setMinimumDuration(1000);
    m_dlg->setAutoClose(true);
    m_dlg->setRange(0, 100);
    m_dlg->exec();
    while (m_process->isRunning()) {
        readFromStdout();
        usleep(50000);
    }
    delete m_process;
    m_process = 0;
    if (m_result == true) {
        const uint newSize = QFileInfo(m_filePath).size();
        const uint decrease = 100 - 100 * newSize / origSize;
        QMessageBox::information(0, QString(),
            QObject::tr("The database has been compacted. Current size decreased by %1% to %2.")
                .arg(decrease).arg(QLocale().toString(double(newSize)/1000000.0, 'f', 2) + " MB"));
    }
    return m_result;
}

void SQLiteVacuum::readFromStdout()
{
    while (true) {
        QString s(m_process->readLineStdout());   //readStdout();
        if (s.isEmpty())
            break;
        m_dlg->setValue(m_percent);
//  PreDrvDbg << m_percent << " " << s;
        if (s.startsWith("VACUUM: ")) {
            //set previously known progress
            m_dlg->setValue(m_percent);
            //update progress info
            if (s.mid(8, 4) == "100%") {
                m_percent = 100;
//TODO?                m_dlg->setAllowCancel(false);
                m_dlg->setCursor(QCursor(Qt::WaitCursor));
            } else if (s.mid(9, 1) == "%") {
                m_percent = s.mid(8, 1).toInt();
            } else if (s.mid(10, 1) == "%") {
                m_percent = s.mid(8, 2).toInt();
            }
            m_process->writeToStdin(QByteArray(" "));
        }
    }
}

void SQLiteVacuum::processExited()
{
// PreDrvDbg << sender()->name() << " EXIT";
    m_dlg->close();
    delete m_dlg;
    m_dlg = 0;
}

void SQLiteVacuum::cancelClicked()
{
    if (!m_process->normalExit()) {
        m_process->writeToStdin(QByteArray("q")); //quit
        m_result = cancelled;
    }
}