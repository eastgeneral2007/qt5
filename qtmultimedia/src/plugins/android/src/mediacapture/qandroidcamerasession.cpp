/****************************************************************************
**
** Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the Qt Toolkit.
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

#include "qandroidcamerasession.h"

#include "jcamera.h"
#include "jmultimediautils.h"
#include "qandroidvideooutput.h"
#include "qandroidmediavideoprobecontrol.h"
#include "qandroidmultimediautils.h"
#include <QtConcurrent/qtconcurrentrun.h>
#include <qfile.h>
#include <qguiapplication.h>
#include <qdebug.h>
#include <qvideoframe.h>

QT_BEGIN_NAMESPACE

class DataVideoBuffer : public QAbstractVideoBuffer
{
public:
    DataVideoBuffer(const QByteArray &d)
        : QAbstractVideoBuffer(NoHandle)
        , data(d)
        , mode(NotMapped)
    { }

    MapMode mapMode() const { return mode; }

    uchar *map(MapMode m, int *numBytes, int *bytesPerLine)
    {
        if (mode != NotMapped || m == NotMapped)
            return 0;

        mode = m;

        if (numBytes)
            *numBytes = data.size();

        if (bytesPerLine)
            *bytesPerLine = -1;

        return reinterpret_cast<uchar *>(data.data());
    }

    void unmap() { mode = NotMapped; }

private:
    QByteArray data;
    MapMode mode;
};


QAndroidCameraSession::QAndroidCameraSession(QObject *parent)
    : QObject(parent)
    , m_selectedCamera(0)
    , m_camera(0)
    , m_nativeOrientation(0)
    , m_previewOrientation(0)
    , m_videoOutput(0)
    , m_captureMode(QCamera::CaptureViewfinder)
    , m_state(QCamera::UnloadedState)
    , m_savedState(-1)
    , m_status(QCamera::UnloadedStatus)
    , m_previewStarted(false)
    , m_imageSettingsDirty(true)
    , m_captureDestination(QCameraImageCapture::CaptureToFile)
    , m_captureImageDriveMode(QCameraImageCapture::SingleImageCapture)
    , m_lastImageCaptureId(0)
    , m_readyForCapture(false)
    , m_captureCanceled(false)
    , m_currentImageCaptureId(-1)
{
    if (qApp) {
        connect(qApp, SIGNAL(applicationStateChanged(Qt::ApplicationState)),
                this, SLOT(onApplicationStateChanged(Qt::ApplicationState)));
    }
}

QAndroidCameraSession::~QAndroidCameraSession()
{
    close();
}

void QAndroidCameraSession::setCaptureMode(QCamera::CaptureModes mode)
{
    if (m_captureMode == mode || !isCaptureModeSupported(mode))
        return;

    m_captureMode = mode;
    emit captureModeChanged(m_captureMode);

    if (m_previewStarted && m_captureMode.testFlag(QCamera::CaptureStillImage))
        adjustViewfinderSize(m_imageSettings.resolution());
}

bool QAndroidCameraSession::isCaptureModeSupported(QCamera::CaptureModes mode) const
{
    if (mode & (QCamera::CaptureStillImage & QCamera::CaptureVideo))
        return false;

    return true;
}

void QAndroidCameraSession::setState(QCamera::State state)
{
    // If the application is inactive, the camera shouldn't be started. Save the desired state
    // instead and it will be set when the application becomes active.
    if (qApp->applicationState() != Qt::ApplicationActive) {
        m_savedState = state;
        return;
    }

    if (m_state == state)
        return;

    switch (state) {
    case QCamera::UnloadedState:
        close();
        break;
    case QCamera::LoadedState:
    case QCamera::ActiveState:
        if (!m_camera && !open()) {
            emit error(QCamera::CameraError, QStringLiteral("Failed to open camera"));
            return;
        }
        if (state == QCamera::ActiveState)
            startPreview();
        else if (state == QCamera::LoadedState)
            stopPreview();
        break;
    }

     m_state = state;
     emit stateChanged(m_state);
}

bool QAndroidCameraSession::open()
{
    close();

    m_status = QCamera::LoadingStatus;
    emit statusChanged(m_status);

    m_camera = JCamera::open(m_selectedCamera);

    if (m_camera) {
        connect(m_camera, SIGNAL(pictureExposed()), this, SLOT(onCameraPictureExposed()));
        connect(m_camera, SIGNAL(previewFetched(QByteArray)), this, SLOT(onCameraPreviewFetched(QByteArray)));
        connect(m_camera, SIGNAL(frameFetched(QByteArray)),
                this, SLOT(onCameraFrameFetched(QByteArray)),
                Qt::DirectConnection);
        connect(m_camera, SIGNAL(pictureCaptured(QByteArray)), this, SLOT(onCameraPictureCaptured(QByteArray)));
        connect(m_camera, SIGNAL(previewStarted()), this, SLOT(onCameraPreviewStarted()));
        connect(m_camera, SIGNAL(previewStopped()), this, SLOT(onCameraPreviewStopped()));

        m_nativeOrientation = m_camera->getNativeOrientation();

        // Preview orientation will always match the device natural orientation
        if (m_camera->getFacing() == JCamera::CameraFacingFront)
            m_previewOrientation = 360 - m_nativeOrientation;
        else
            m_previewOrientation = m_nativeOrientation;

        m_status = QCamera::LoadedStatus;

        if (m_camera->getPreviewFormat() != JCamera::NV21)
            m_camera->setPreviewFormat(JCamera::NV21);

        m_camera->fetchEachFrame(m_videoProbes.count());

        emit opened();
    } else {
        m_status = QCamera::UnavailableStatus;
    }

    emit statusChanged(m_status);

    return m_camera != 0;
}

void QAndroidCameraSession::close()
{
    if (!m_camera)
        return;

    stopPreview();

    m_status = QCamera::UnloadingStatus;
    emit statusChanged(m_status);

    m_readyForCapture = false;
    m_currentImageCaptureId = -1;
    m_currentImageCaptureFileName.clear();
    m_imageSettingsDirty = true;

    m_camera->release();
    delete m_camera;
    m_camera = 0;

    m_status = QCamera::UnloadedStatus;
    emit statusChanged(m_status);
}

void QAndroidCameraSession::setVideoPreview(QObject *videoOutput)
{
    if (m_videoOutput)
        m_videoOutput->stop();

    if (videoOutput) {
        connect(videoOutput, SIGNAL(readyChanged(bool)), this, SLOT(onVideoOutputReady(bool)));
        m_videoOutput = qobject_cast<QAndroidVideoOutput *>(videoOutput);
    } else {
        m_videoOutput = 0;
    }
}

void QAndroidCameraSession::adjustViewfinderSize(const QSize &captureSize, bool restartPreview)
{
    if (!m_camera)
        return;

    QSize viewfinderResolution = m_camera->previewSize();
    const qreal aspectRatio = qreal(captureSize.width()) / qreal(captureSize.height());
    if (viewfinderResolution.isValid() &&
            qFuzzyCompare(aspectRatio,
                          qreal(viewfinderResolution.width()) / viewfinderResolution.height())) {
        return;
    }

    QList<QSize> previewSizes = m_camera->getSupportedPreviewSizes();
    for (int i = previewSizes.count() - 1; i >= 0; --i) {
        const QSize &size = previewSizes.at(i);
        // search for viewfinder resolution with the same aspect ratio
        if (qFuzzyCompare(aspectRatio, (static_cast<qreal>(size.width())/static_cast<qreal>(size.height())))) {
            viewfinderResolution = size;
            break;
        }
    }

    if (m_camera->previewSize() != viewfinderResolution) {
        if (m_videoOutput) {
            QSize size = viewfinderResolution;

            // If the preview orientation is not the defaut one (0 or 180 degrees),
            // we have to invert the output aspect ratio.
            if (m_previewOrientation % 180)
                size.transpose();

            m_videoOutput->setVideoSize(size);
        }

        // if preview is started, we have to stop it first before changing its size
        if (m_previewStarted && restartPreview)
            m_camera->stopPreview();

        m_camera->setPreviewSize(viewfinderResolution);

        // restart preview
        if (m_previewStarted && restartPreview)
            m_camera->startPreview();
    }
}

void QAndroidCameraSession::startPreview()
{
    if (!m_camera || m_previewStarted)
        return;

    m_status = QCamera::StartingStatus;
    emit statusChanged(m_status);

    applyImageSettings();
    adjustViewfinderSize(m_imageSettings.resolution());
    m_camera->setDisplayOrientation(m_previewOrientation);

    if (m_videoOutput && m_videoOutput->isReady())
        onVideoOutputReady(true);

    JMultimediaUtils::enableOrientationListener(true);

    m_camera->startPreview();
    m_previewStarted = true;
}

void QAndroidCameraSession::stopPreview()
{
    if (!m_camera || !m_previewStarted)
        return;

    m_status = QCamera::StoppingStatus;
    emit statusChanged(m_status);

    JMultimediaUtils::enableOrientationListener(false);

    m_camera->stopPreview();
    m_camera->setPreviewSize(QSize());
    if (m_videoOutput)
        m_videoOutput->stop();
    m_previewStarted = false;
}

void QAndroidCameraSession::setImageSettings(const QImageEncoderSettings &settings)
{
    if (m_imageSettings == settings)
        return;

    m_imageSettings = settings;
    if (m_imageSettings.codec().isEmpty())
        m_imageSettings.setCodec(QLatin1String("jpeg"));

    m_imageSettingsDirty = true;

    applyImageSettings();

    if (m_readyForCapture && m_captureMode.testFlag(QCamera::CaptureStillImage))
        adjustViewfinderSize(m_imageSettings.resolution());
}

int QAndroidCameraSession::currentCameraRotation() const
{
    if (!m_camera)
        return 0;

    // subtract natural camera orientation and physical device orientation
    int rotation = 0;
    int deviceOrientation = (JMultimediaUtils::getDeviceOrientation() + 45) / 90 * 90;
    if (m_camera->getFacing() == JCamera::CameraFacingFront)
        rotation = (m_nativeOrientation - deviceOrientation + 360) % 360;
    else // back-facing camera
        rotation = (m_nativeOrientation + deviceOrientation) % 360;

    return rotation;
}

void QAndroidCameraSession::addProbe(QAndroidMediaVideoProbeControl *probe)
{
    m_videoProbesMutex.lock();
    if (probe)
        m_videoProbes << probe;
    if (m_camera)
        m_camera->fetchEachFrame(m_videoProbes.count());
    m_videoProbesMutex.unlock();
}

void QAndroidCameraSession::removeProbe(QAndroidMediaVideoProbeControl *probe)
{
    m_videoProbesMutex.lock();
    m_videoProbes.remove(probe);
    if (m_camera)
        m_camera->fetchEachFrame(m_videoProbes.count());
    m_videoProbesMutex.unlock();
}

void QAndroidCameraSession::applyImageSettings()
{
    if (!m_camera || !m_imageSettingsDirty)
        return;

    const QSize requestedResolution = m_imageSettings.resolution();
    const QList<QSize> supportedResolutions = m_camera->getSupportedPictureSizes();

    if (!requestedResolution.isValid()) {
        // if no resolution is set, use the highest supported one
        m_imageSettings.setResolution(supportedResolutions.last());
    } else if (!supportedResolutions.contains(requestedResolution)) {
        // if the requested resolution is not supported, find the closest one
        int reqPixelCount = requestedResolution.width() * requestedResolution.height();
        QList<int> supportedPixelCounts;
        for (int i = 0; i < supportedResolutions.size(); ++i) {
            const QSize &s = supportedResolutions.at(i);
            supportedPixelCounts.append(s.width() * s.height());
        }
        int closestIndex = qt_findClosestValue(supportedPixelCounts, reqPixelCount);
        m_imageSettings.setResolution(supportedResolutions.at(closestIndex));
    }

    int jpegQuality = 100;
    switch (m_imageSettings.quality()) {
    case QMultimedia::VeryLowQuality:
        jpegQuality = 20;
        break;
    case QMultimedia::LowQuality:
        jpegQuality = 40;
        break;
    case QMultimedia::NormalQuality:
        jpegQuality = 60;
        break;
    case QMultimedia::HighQuality:
        jpegQuality = 80;
        break;
    case QMultimedia::VeryHighQuality:
        jpegQuality = 100;
        break;
    }

    m_camera->setPictureSize(m_imageSettings.resolution());
    m_camera->setJpegQuality(jpegQuality);

    m_imageSettingsDirty = false;
}

bool QAndroidCameraSession::isCaptureDestinationSupported(QCameraImageCapture::CaptureDestinations destination) const
{
    return destination & (QCameraImageCapture::CaptureToFile | QCameraImageCapture::CaptureToBuffer);
}

QCameraImageCapture::CaptureDestinations QAndroidCameraSession::captureDestination() const
{
    return m_captureDestination;
}

void QAndroidCameraSession::setCaptureDestination(QCameraImageCapture::CaptureDestinations destination)
{
    if (m_captureDestination != destination) {
        m_captureDestination = destination;
        emit captureDestinationChanged(m_captureDestination);
    }
}

bool QAndroidCameraSession::isReadyForCapture() const
{
    return m_status == QCamera::ActiveStatus && m_readyForCapture;
}

void QAndroidCameraSession::setReadyForCapture(bool ready)
{
    if (m_readyForCapture == ready)
        return;

    m_readyForCapture = ready;
    emit readyForCaptureChanged(ready);
}

QCameraImageCapture::DriveMode QAndroidCameraSession::driveMode() const
{
    return m_captureImageDriveMode;
}

void QAndroidCameraSession::setDriveMode(QCameraImageCapture::DriveMode mode)
{
    m_captureImageDriveMode = mode;
}

int QAndroidCameraSession::capture(const QString &fileName)
{
    ++m_lastImageCaptureId;

    if (!isReadyForCapture()) {
        emit imageCaptureError(m_lastImageCaptureId, QCameraImageCapture::NotReadyError,
                               tr("Camera not ready"));
        return m_lastImageCaptureId;
    }

    if (m_captureImageDriveMode == QCameraImageCapture::SingleImageCapture) {
        setReadyForCapture(false);

        m_currentImageCaptureId = m_lastImageCaptureId;
        m_currentImageCaptureFileName = fileName;

        applyImageSettings();
        adjustViewfinderSize(m_imageSettings.resolution());

        // adjust picture rotation depending on the device orientation
        m_camera->setRotation(currentCameraRotation());

        m_camera->takePicture();
    } else {
        //: Drive mode is the camera's shutter mode, for example single shot, continuos exposure, etc.
        emit imageCaptureError(m_lastImageCaptureId, QCameraImageCapture::NotSupportedFeatureError,
                               tr("Drive mode not supported"));
    }

    return m_lastImageCaptureId;
}

void QAndroidCameraSession::cancelCapture()
{
    if (m_readyForCapture)
        return;

    m_captureCanceled = true;
}

void QAndroidCameraSession::onCameraPictureExposed()
{
    if (m_captureCanceled)
        return;

    emit imageExposed(m_currentImageCaptureId);
    m_camera->fetchLastPreviewFrame();
}

void QAndroidCameraSession::onCameraPreviewFetched(const QByteArray &preview)
{
    if (preview.size()) {
        QtConcurrent::run(this, &QAndroidCameraSession::processPreviewImage,
                          m_currentImageCaptureId,
                          preview,
                          m_camera->getRotation());
    }
}

void QAndroidCameraSession::onCameraFrameFetched(const QByteArray &frame)
{
    m_videoProbesMutex.lock();
    if (frame.size() && m_videoProbes.count()) {
        QVideoFrame videoFrame(new DataVideoBuffer(frame),
                               m_camera->previewSize(),
                               QVideoFrame::Format_NV21);
        foreach (QAndroidMediaVideoProbeControl *probe, m_videoProbes)
            probe->newFrameProbed(videoFrame);
    }
    m_videoProbesMutex.unlock();
}

void QAndroidCameraSession::onCameraPictureCaptured(const QByteArray &data)
{
    if (!m_captureCanceled) {
        // Loading and saving the captured image can be slow, do it in a separate thread
        QtConcurrent::run(this, &QAndroidCameraSession::processCapturedImage,
                          m_currentImageCaptureId,
                          data,
                          m_imageSettings.resolution(),
                          m_captureDestination,
                          m_currentImageCaptureFileName);
    }

    m_captureCanceled = false;

    // Preview needs to be restarted after taking a picture
    m_camera->startPreview();
}

void QAndroidCameraSession::onCameraPreviewStarted()
{
    if (m_status == QCamera::StartingStatus) {
        m_status = QCamera::ActiveStatus;
        emit statusChanged(m_status);
    }

    setReadyForCapture(true);
}

void QAndroidCameraSession::onCameraPreviewStopped()
{
    if (m_status == QCamera::StoppingStatus) {
        m_status = QCamera::LoadedStatus;
        emit statusChanged(m_status);
    }

    setReadyForCapture(false);
}

void QAndroidCameraSession::processCapturedImage(int id,
                                                 const QByteArray &data,
                                                 const QSize &resolution,
                                                 QCameraImageCapture::CaptureDestinations dest,
                                                 const QString &fileName)
{


    if (dest & QCameraImageCapture::CaptureToFile) {
        const QString actualFileName = m_mediaStorageLocation.generateFileName(fileName,
                                                                               QAndroidMediaStorageLocation::Camera,
                                                                               QLatin1String("IMG_"),
                                                                               QLatin1String("jpg"));

        QFile file(actualFileName);
        if (file.open(QFile::WriteOnly)) {
            if (file.write(data) == data.size()) {
                // if the picture is saved into the standard picture location, register it
                // with the Android media scanner so it appears immediately in apps
                // such as the gallery.
                QString standardLoc = JMultimediaUtils::getDefaultMediaDirectory(JMultimediaUtils::DCIM);
                if (actualFileName.startsWith(standardLoc))
                    JMultimediaUtils::registerMediaFile(actualFileName);

                emit imageSaved(id, actualFileName);
            } else {
                emit imageCaptureError(id, QCameraImageCapture::OutOfSpaceError, file.errorString());
            }
        } else {
            const QString errorMessage = tr("Could not open destination file: %1").arg(actualFileName);
            emit imageCaptureError(id, QCameraImageCapture::ResourceError, errorMessage);
        }
    }

    if (dest & QCameraImageCapture::CaptureToBuffer) {
        QVideoFrame frame(new DataVideoBuffer(data), resolution, QVideoFrame::Format_Jpeg);
        emit imageAvailable(id, frame);
    }
}

void QAndroidCameraSession::processPreviewImage(int id, const QByteArray &data, int rotation)
{
    emit imageCaptured(id, prepareImageFromPreviewData(data, rotation));
}

QImage QAndroidCameraSession::prepareImageFromPreviewData(const QByteArray &data, int rotation)
{
    QSize frameSize = m_camera->previewSize();
    QImage result(frameSize, QImage::Format_ARGB32);
    qt_convert_NV21_to_ARGB32((const uchar *)data.constData(),
                              (quint32 *)result.bits(),
                              frameSize.width(),
                              frameSize.height());

    QTransform transform;

    // Preview display of front-facing cameras is flipped horizontally, but the frame data
    // we get here is not. Flip it ourselves if the camera is front-facing to match what the user
    // sees on the viewfinder.
    if (m_camera->getFacing() == JCamera::CameraFacingFront)
        transform.scale(-1, 1);

    transform.rotate(rotation);

    result = result.transformed(transform);

    return result;
}

void QAndroidCameraSession::onVideoOutputReady(bool ready)
{
    if (m_camera && m_videoOutput && ready)
        m_camera->setPreviewTexture(m_videoOutput->surfaceTexture());
}

void QAndroidCameraSession::onApplicationStateChanged(Qt::ApplicationState state)
{
    switch (state) {
    case Qt::ApplicationInactive:
        if (m_state != QCamera::UnloadedState) {
            m_savedState = m_state;
            close();
            m_state = QCamera::UnloadedState;
            emit stateChanged(m_state);
        }
        break;
    case Qt::ApplicationActive:
        if (m_savedState != -1) {
            setState(QCamera::State(m_savedState));
            m_savedState = -1;
        }
        break;
    default:
        break;
    }
}

QT_END_NAMESPACE
