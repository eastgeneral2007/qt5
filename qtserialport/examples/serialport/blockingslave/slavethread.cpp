/****************************************************************************
**
** Copyright (C) 2012 Denis Shienkov <denis.shienkov@gmail.com>
** Contact: http://www.qt-project.org/legal
**
** This file is part of the QtSerialPort module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "slavethread.h"

#include <QtSerialPort/QSerialPort>

#include <QTime>

QT_USE_NAMESPACE

SlaveThread::SlaveThread(QObject *parent)
    : QThread(parent), waitTimeout(0), quit(false)
{
}
//! [0]
SlaveThread::~SlaveThread()
{
    mutex.lock();
    quit = true;
    mutex.unlock();
    wait();
}
//! [0]

//! [1] //! [2]
void SlaveThread::startSlave(const QString &portName, int waitTimeout, const QString &response)
{
//! [1]
    QMutexLocker locker(&mutex);
    this->portName = portName;
    this->waitTimeout = waitTimeout;
    this->response = response;
//! [3]
    if (!isRunning())
        start();
}
//! [2] //! [3]

//! [4]
void SlaveThread::run()
{
    bool currentPortNameChanged = false;

    mutex.lock();
//! [4] //! [5]
    QString currentPortName;
    if (currentPortName != portName) {
        currentPortName = portName;
        currentPortNameChanged = true;
    }

    int currentWaitTimeout = waitTimeout;
    QString currentRespone = response;
    mutex.unlock();
//! [5] //! [6]
    QSerialPort serial;

    while (!quit) {
//![6] //! [7]
        if (currentPortNameChanged) {
            serial.close();
            serial.setPortName(currentPortName);

            if (!serial.open(QIODevice::ReadWrite)) {
                emit error(tr("Can't open %1, error code %2")
                           .arg(portName).arg(serial.error()));
                return;
            }

            if (!serial.setBaudRate(QSerialPort::Baud9600)) {
                emit error(tr("Can't set baud rate 9600 baud to port %1, error code %2")
                           .arg(portName).arg(serial.error()));
                return;
            }

            if (!serial.setDataBits(QSerialPort::Data8)) {
                emit error(tr("Can't set 8 data bits to port %1, error code %2")
                           .arg(portName).arg(serial.error()));
                return;
            }

            if (!serial.setParity(QSerialPort::NoParity)) {
                emit error(tr("Can't set no parity to port %1, error code %2")
                           .arg(portName).arg(serial.error()));
                return;
            }

            if (!serial.setStopBits(QSerialPort::OneStop)) {
                emit error(tr("Can't set 1 stop bit to port %1, error code %2")
                           .arg(portName).arg(serial.error()));
                return;
            }

            if (!serial.setFlowControl(QSerialPort::NoFlowControl)) {
                emit error(tr("Can't set no flow control to port %1, error code %2")
                           .arg(portName).arg(serial.error()));
                return;
            }
        }

        if (serial.waitForReadyRead(currentWaitTimeout)) {
//! [7] //! [8]
            // read request
            QByteArray requestData = serial.readAll();
            while (serial.waitForReadyRead(10))
                requestData += serial.readAll();
//! [8] //! [10]
            // write response
            QByteArray responseData = currentRespone.toLocal8Bit();
            serial.write(responseData);
            if (serial.waitForBytesWritten(waitTimeout)) {
                QString request(requestData);
//! [12]
                emit this->request(request);
//! [10] //! [11] //! [12]
            } else {
                emit timeout(tr("Wait write response timeout %1")
                             .arg(QTime::currentTime().toString()));
            }
//! [9] //! [11]
        } else {
            emit timeout(tr("Wait read request timeout %1")
                         .arg(QTime::currentTime().toString()));
        }
//! [9]  //! [13]
        mutex.lock();
        if (currentPortName != portName) {
            currentPortName = portName;
            currentPortNameChanged = true;
        } else {
            currentPortNameChanged = false;
        }
        currentWaitTimeout = waitTimeout;
        currentRespone = response;
        mutex.unlock();
    }
//! [13]
}
