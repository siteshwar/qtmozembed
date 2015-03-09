/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-*/
/* vim: set ts=2 sw=2 et tw=79: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#define LOG_COMPONENT "QGraphicsMozViewPrivate"

#include <QTouchEvent>
#include <QJsonDocument>
#include <QGuiApplication>

#include "qgraphicsmozview_p.h"
#include "qmozcontext.h"
#include "InputData.h"
#include "mozilla/embedlite/EmbedLiteApp.h"
#include "mozilla/gfx/Tools.h"
#include "qmozembedlog.h"
#include <sys/time.h>
#include "mozilla/TimeStamp.h"

#ifndef MOZVIEW_FLICK_THRESHOLD
#define MOZVIEW_FLICK_THRESHOLD 200
#endif

#define SCROLL_EPSILON 0.001

using namespace mozilla;
using namespace mozilla::embedlite;

qint64 current_timestamp(QTouchEvent* aEvent)
{
    if (aEvent) {
        return aEvent->timestamp();
    }

    struct timeval te;
    gettimeofday(&te, NULL);
    qint64 milliseconds = te.tv_sec*1000LL + te.tv_usec/1000;
    return milliseconds;
}

QGraphicsMozViewPrivate::QGraphicsMozViewPrivate(IMozQViewIface* aViewIface)
    : mViewIface(aViewIface)
    , mContext(NULL)
    , mView(NULL)
    , mViewInitialized(false)
    , mBgColor(Qt::white)
    , mTempTexture(NULL)
    , mEnabled(true)
    , mChromeGestureEnabled(true)
    , mChromeGestureThreshold(0.0)
    , mChrome(true)
    , mMoveDelta(0.0)
    , mDragStartY(0.0)
    , mMoving(false)
    , mPinching(false)
    , mLastTimestamp(0)
    , mLastStationaryTimestamp(0)
    , mCanFlick(false)
    , mPendingTouchEvent(false)
    , mProgress(0)
    , mCanGoBack(false)
    , mCanGoForward(false)
    , mIsLoading(false)
    , mLastIsGoodRotation(true)
    , mIsPasswordField(false)
    , mGraphicsViewAssigned(false)
    , mContentRect(0.0, 0.0, 0.0, 0.0)
    , mScrollableSize(0.0, 0.0)
    , mScrollableOffset(0,0)
    , mContentResolution(0.0)
    , mIsPainted(false)
    , mInputMethodHints(0)
    , mIsInputFieldFocused(false)
    , mViewIsFocused(false)
    , mHasContext(false)
    , mGLSurfaceSize(0,0)
    , mPressed(false)
    , mDragging(false)
    , mFlicking(false)
{
}

QGraphicsMozViewPrivate::~QGraphicsMozViewPrivate()
{
    delete mViewIface;
}

void QGraphicsMozViewPrivate::CompositorCreated()
{
    mViewIface->createGeckoGLContext();
}

void QGraphicsMozViewPrivate::UpdateScrollArea(unsigned int aWidth, unsigned int aHeight, float aPosX, float aPosY)
{
    bool widthChanged = false;
    bool heightChanged = false;
    // Emit changes only after both values have been updated.
    if (mScrollableSize.width() != aWidth) {
        mScrollableSize.setWidth(aWidth);
        widthChanged = true;
    }

    if (mScrollableSize.height() != aHeight) {
        mScrollableSize.setHeight(aHeight);
        heightChanged = true;
    }

    if (!gfx::FuzzyEqual(mScrollableOffset.x(), aPosX, SCROLL_EPSILON) ||
        !gfx::FuzzyEqual(mScrollableOffset.y(), aPosY, SCROLL_EPSILON)) {

        mScrollableOffset.setX(aPosX);
        mScrollableOffset.setY(aPosY);
        mViewIface->scrollableOffsetChanged();

        if (mEnabled) {
            // We could add moving timers for both of these and check them separately.
            // Currently we have only one timer event for content.
            mVerticalScrollDecorator.setMoving(true);
            mHorizontalScrollDecorator.setMoving(true);

            // Update vertical scroll decorator
            qreal ySizeRatio = mContentRect.height() * mContentResolution / mScrollableSize.height();
            qreal tmpValue = mSize.height() * ySizeRatio;
            mVerticalScrollDecorator.setSize(tmpValue);
            tmpValue = mScrollableOffset.y() * ySizeRatio;
            mVerticalScrollDecorator.setPosition(tmpValue);

            // Update horizontal scroll decorator
            qreal xSizeRatio = mContentRect.width() * mContentResolution / mScrollableSize.width();
            tmpValue = mSize.width() * xSizeRatio;
            mHorizontalScrollDecorator.setSize(tmpValue);
            tmpValue = mScrollableOffset.x() * xSizeRatio;
            mHorizontalScrollDecorator.setPosition(tmpValue);
        }
    }

    if (widthChanged) {
        mViewIface->contentWidthChanged();
    }

    if (heightChanged) {
        mViewIface->contentHeightChanged();
    }
}

void QGraphicsMozViewPrivate::TestFlickingMode(QTouchEvent *event)
{
    QPointF touchPoint = event->touchPoints().size() == 1 ? event->touchPoints().at(0).pos() : QPointF();
    // Only for single press point
    if (!touchPoint.isNull()) {
        if (event->type() == QEvent::TouchBegin) {
            mLastTimestamp = mLastStationaryTimestamp = current_timestamp(event);
            mCanFlick = true;
        } else if (event->type() == QEvent::TouchUpdate && !mLastPos.isNull()) {
            QRectF pressArea = event->touchPoints().at(0).rect();
            qreal touchHorizontalThreshold = pressArea.width() * 2;
            qreal touchVerticalThreshold = pressArea.height() * 2;
            if (!mLastStationaryPos.isNull() && (qAbs(mLastStationaryPos.x() - touchPoint.x()) > touchHorizontalThreshold
                                             || qAbs(mLastStationaryPos.y() - touchPoint.y()) > touchVerticalThreshold)) {
                // Threshold exceeded. Reset stationary position and time.
                mLastStationaryTimestamp = current_timestamp(event);
                mLastStationaryPos = touchPoint;
            } else if (qAbs(mLastPos.x() - touchPoint.x()) <= touchHorizontalThreshold && qAbs(mLastPos.y() - touchPoint.y()) <= touchVerticalThreshold) {
                // Handle stationary position when panning stops and continues. Eventually mCanFlick is based on timestamps between events, see touch end block.
                if (mCanFlick) {
                    mLastStationaryTimestamp = current_timestamp(event);
                    mLastStationaryPos = touchPoint;
                }
                mCanFlick = false;
            }
            else {
                mCanFlick = true;
            }
            mLastTimestamp = current_timestamp(event);
        } else if (event->type() == QEvent::TouchEnd) {
            mCanFlick =(qint64(current_timestamp(event) - mLastTimestamp) < MOZVIEW_FLICK_THRESHOLD) &&
                    (qint64(current_timestamp(event) - mLastStationaryTimestamp) < MOZVIEW_FLICK_THRESHOLD);
            mLastStationaryPos = QPointF();
        }
    }
    mLastPos = touchPoint;
}

void QGraphicsMozViewPrivate::HandleTouchEnd(bool &draggingChanged, bool &pinchingChanged)
{
    if (mDragging) {
        mDragging = false;
        draggingChanged = true;
    }

    // Currently change from 2> fingers to 1 finger does not
    // allow moving content. Hence, keep pinching enabled
    // also when there is one finger left when releasing
    // fingers and only stop pinching when touch ends.
    // You can continue pinching by adding second finger.
    if (mPinching) {
        mPinching = false;
        pinchingChanged = true;
    }
}

void QGraphicsMozViewPrivate::ResetState()
{
    // Invalid initial drag start Y.
    mDragStartY = -1.0;
    mMoveDelta = 0.0;

    mFlicking = false;
    UpdateMoving(false);
    mVerticalScrollDecorator.setMoving(false);
    mHorizontalScrollDecorator.setMoving(false);
}

void QGraphicsMozViewPrivate::UpdateMoving(bool moving)
{
    if (mMoving != moving) {
        mMoving = moving;
        mViewIface->movingChanged();
    }
}

void QGraphicsMozViewPrivate::ResetPainted()
{
    if (mIsPainted) {
        mIsPainted = false;
        mViewIface->firstPaint(-1, -1);
    }
}

void QGraphicsMozViewPrivate::UpdateViewSize()
{
    if (mSize.isEmpty())
        return;

    if (!mViewInitialized) {
        return;
    }

    if (mContext->GetApp()->IsAccelerated() && mHasContext) {
        mView->SetGLViewPortSize(mGLSurfaceSize.width(), mGLSurfaceSize.height());
    }
    mView->SetViewSize(mSize.width(), mSize.height());
}

bool QGraphicsMozViewPrivate::RequestCurrentGLContext()
{
    QSize unused;
    return RequestCurrentGLContext(unused);
}

bool QGraphicsMozViewPrivate::RequestCurrentGLContext(QSize& aViewPortSize)
{
    bool hasContext = false;
    mViewIface->requestGLContext(hasContext, aViewPortSize);
    return hasContext;
}

void QGraphicsMozViewPrivate::ViewInitialized()
{
    mViewInitialized = true;
    UpdateViewSize();
    // This is currently part of official API, so let's subscribe to these messages by default
    mViewIface->viewInitialized();
    mViewIface->navigationHistoryChanged();
}

void QGraphicsMozViewPrivate::SetBackgroundColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    mBgColor = QColor(r, g, b, a);
    mViewIface->bgColorChanged();
}

void QGraphicsMozViewPrivate::CompositingFinished()
{
    mViewIface->CompositingFinished();
}

void QGraphicsMozViewPrivate::OnLocationChanged(const char* aLocation, bool aCanGoBack, bool aCanGoForward)
{
    if (mCanGoBack != aCanGoBack || mCanGoForward != aCanGoForward) {
        mCanGoBack = aCanGoBack;
        mCanGoForward = aCanGoForward;
        mViewIface->navigationHistoryChanged();
    }

    if (mLocation != aLocation) {
        mLocation = QString(aLocation);
        mViewIface->urlChanged();
    }
}

void QGraphicsMozViewPrivate::OnLoadProgress(int32_t aProgress, int32_t aCurTotal, int32_t aMaxTotal)
{
    if (mIsLoading) {
        mProgress = aProgress;
        mViewIface->loadProgressChanged();
    }
}

void QGraphicsMozViewPrivate::OnLoadStarted(const char* aLocation)
{
    Q_UNUSED(aLocation);

    ResetPainted();

    if (!mIsLoading) {
        mIsLoading = true;
        mProgress = 1;
        mViewIface->loadingChanged();
    }
}

void QGraphicsMozViewPrivate::OnLoadFinished(void)
{
    if (mIsLoading) {
        mProgress = 100;
        mIsLoading = false;
        mViewIface->loadingChanged();
    }
}

void QGraphicsMozViewPrivate::OnWindowCloseRequested()
{
    mViewIface->windowCloseRequested();
}

// View finally destroyed and deleted
void QGraphicsMozViewPrivate::ViewDestroyed()
{
    LOGT();
    mView = NULL;
    mViewInitialized = false;
    mViewIface->viewDestroyed();
}

void QGraphicsMozViewPrivate::RecvAsyncMessage(const char16_t* aMessage, const char16_t* aData)
{
    NS_ConvertUTF16toUTF8 message(aMessage);
    NS_ConvertUTF16toUTF8 data(aData);

    bool ok = false;
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray(data.get()), &error);
    ok = error.error == QJsonParseError::NoError;
    QVariant vdata = doc.toVariant();

    if (ok) {
        LOGT("mesg:%s, data:%s", message.get(), data.get());
        mViewIface->recvAsyncMessage(message.get(), vdata);
    } else {
        LOGT("parse: err:%s, errLine:%i", error.errorString().toUtf8().data(), error.offset);
    }
}

char* QGraphicsMozViewPrivate::RecvSyncMessage(const char16_t* aMessage, const char16_t*  aData)
{
    QMozReturnValue response;
    NS_ConvertUTF16toUTF8 message(aMessage);
    NS_ConvertUTF16toUTF8 data(aData);

    bool ok = false;
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray(data.get()), &error);
    ok = error.error == QJsonParseError::NoError;
    QVariant vdata = doc.toVariant();

    mViewIface->recvSyncMessage(message.get(), vdata, &response);

    QJsonDocument respdoc = QJsonDocument::fromVariant(response.getMessage());
    QByteArray array = respdoc.toJson();

    LOGT("msg:%s, response:%s", message.get(), array.constData());
    return strdup(array.constData());
}

void QGraphicsMozViewPrivate::OnLoadRedirect(void)
{
    LOGT();
    mViewIface->loadRedirect();
}

void QGraphicsMozViewPrivate::OnSecurityChanged(const char* aStatus, unsigned int aState)
{
    LOGT();
    mViewIface->securityChanged(aStatus, aState);
}
void QGraphicsMozViewPrivate::OnFirstPaint(int32_t aX, int32_t aY)
{
    LOGT();
    mIsPainted = true;
    mViewIface->firstPaint(aX, aY);
}

void QGraphicsMozViewPrivate::SetIsFocused(bool aIsFocused)
{
    mViewIsFocused = aIsFocused;
    if (mViewInitialized) {
        mView->SetIsFocused(aIsFocused);
    }
}

void QGraphicsMozViewPrivate::IMENotification(int aIstate, bool aOpen, int aCause, int aFocusChange,
                                              const char16_t* inputType, const char16_t* inputMode)
{
    Qt::InputMethodHints hints = Qt::ImhNone;
    hints = aIstate == 2 ? Qt::ImhHiddenText : Qt::ImhPreferLowercase;

    QString imType((QChar*)inputType);
    if (imType.contains("number", Qt::CaseInsensitive)) {
        //hints |= Qt::ImhDigitsOnly;
        hints |= Qt::ImhFormattedNumbersOnly;
    }
    else if (imType.contains("tel", Qt::CaseInsensitive)) {
        hints |= Qt::ImhDialableCharactersOnly;
    }
    else if (imType.contains("email", Qt::CaseInsensitive)) {
        hints |= Qt::ImhEmailCharactersOnly;
    }
    else if (imType.contains("url", Qt::CaseInsensitive)) {
        hints |= Qt::ImhUrlCharactersOnly;
    }

    mViewIface->setInputMethodHints(hints);
    if (aFocusChange) {
        mIsInputFieldFocused = aIstate;
        if (mViewIsFocused) {
#ifndef QT_NO_IM
            QInputMethod* inputContext = qGuiApp->inputMethod();
            if (!inputContext) {
                LOGT("Requesting SIP: but no input context");
                return;
            }
            inputContext->update(Qt::ImEnabled);
            if (aIstate) {
                inputContext->show();
            } else {
                inputContext->hide();
            }
            inputContext->update(Qt::ImQueryAll);
#endif
        }

    }
    mViewIface->imeNotification(aIstate, aOpen, aCause, aFocusChange, imType);
}

void QGraphicsMozViewPrivate::GetIMEStatus(int32_t* aIMEEnabled, int32_t* aIMEOpen, intptr_t* aNativeIMEContext)
{
    *aNativeIMEContext = (intptr_t)qApp->inputMethod();
}

void QGraphicsMozViewPrivate::OnScrolledAreaChanged(unsigned int aWidth, unsigned int aHeight)
{
    LOGT("sz[%u,%u]", aWidth, aHeight);
    Q_UNUSED(aWidth)
    Q_UNUSED(aHeight)
}

void QGraphicsMozViewPrivate::OnScrollChanged(int32_t offSetX, int32_t offSetY)
{
}

void QGraphicsMozViewPrivate::OnTitleChanged(const char16_t* aTitle)
{
    mTitle = QString((QChar*)aTitle);
    mViewIface->titleChanged();
}

void QGraphicsMozViewPrivate::SetFirstPaintViewport(const nsIntPoint& aOffset, float aZoom,
                                                    const nsIntRect& aPageRect, const gfxRect& aCssPageRect)
{
    LOGT();
}

void QGraphicsMozViewPrivate::SyncViewportInfo(const nsIntRect& aDisplayPort,
                                               float aDisplayResolution, bool aLayersUpdated,
                                               nsIntPoint& aScrollOffset, float& aScaleX, float& aScaleY)
{
    LOGT("viewport display port[%d,%d,%d,%d]", aDisplayPort.x, aDisplayPort.y, aDisplayPort.width, aDisplayPort.height);
}

void QGraphicsMozViewPrivate::SetPageRect(const gfxRect& aCssPageRect)
{
    LOGT();
}

bool QGraphicsMozViewPrivate::SendAsyncScrollDOMEvent(const gfxRect& aContentRect, const gfxSize& aScrollableSize)
{
    mContentResolution = mSize.width() / aContentRect.width;

    if (mContentRect.x() != aContentRect.x || mContentRect.y() != aContentRect.y ||
            mContentRect.width() != aContentRect.width ||
            mContentRect.height() != aContentRect.height) {
        mContentRect.setRect(aContentRect.x, aContentRect.y, aContentRect.width, aContentRect.height);
        mViewIface->viewAreaChanged();
        // chrome, chromeGestureEnabled, and chromeGestureThreshold can be used
        // to control chrome/chromeless mode.
        // When chromeGestureEnabled is false, no actions are taken
        // When chromeGestureThreshold is true, chrome is set false when chromeGestrureThreshold is exceeded (pan/flick)
        // and set to true when flicking/panning the same amount to the the opposite direction.
        // This do not have relationship to HTML5 fullscreen API.
        if (mEnabled && mChromeGestureEnabled && mDragStartY >= 0.0) {
            // In MozView coordinates
            qreal offset = aContentRect.y * mContentResolution;
            qreal currentDelta = offset - mDragStartY;
            LOGT("dragStartY: %f, %f, %f, %f, %d", mDragStartY, offset, currentDelta, mMoveDelta, (qAbs(currentDelta) < mMoveDelta));

            if (qAbs(currentDelta) < mMoveDelta) {
                mDragStartY = offset;
            }

            if (currentDelta > mChromeGestureThreshold) {
                LOGT("currentDelta > mChromeGestureThreshold: %d", mChrome);
                if (mChrome) {
                    mChrome = false;
                    mViewIface->chromeChanged();
                }
            } else if (currentDelta < -mChromeGestureThreshold) {
                LOGT("currentDelta < -mChromeGestureThreshold: %d", mChrome);
                if (!mChrome) {
                    mChrome = true;
                    mViewIface->chromeChanged();
                }
            }
            mMoveDelta = qAbs(currentDelta);
        }
    }


    UpdateScrollArea(aScrollableSize.width * mContentResolution, aScrollableSize.height * mContentResolution,
                     aContentRect.x * mContentResolution, aContentRect.y * mContentResolution);
    return false;
}

bool QGraphicsMozViewPrivate::HandleLongTap(const nsIntPoint& aPoint)
{
    QMozReturnValue retval;
    retval.setMessage(false);
    mViewIface->handleLongTap(QPoint(aPoint.x, aPoint.y), &retval);
    return retval.getMessage().toBool();
}

bool QGraphicsMozViewPrivate::HandleSingleTap(const nsIntPoint& aPoint)
{
    QMozReturnValue retval;
    retval.setMessage(false);
    mViewIface->handleSingleTap(QPoint(aPoint.x, aPoint.y), &retval);
    return retval.getMessage().toBool();
}

bool QGraphicsMozViewPrivate::HandleDoubleTap(const nsIntPoint& aPoint)
{
    QMozReturnValue retval;
    retval.setMessage(false);
    mViewIface->handleDoubleTap(QPoint(aPoint.x, aPoint.y), &retval);
    return retval.getMessage().toBool();
}

void QGraphicsMozViewPrivate::touchEvent(QTouchEvent* event)
{
    // Always accept the QTouchEvent so that we'll receive also TouchUpdate and TouchEnd events
    mPendingTouchEvent = true;
    event->setAccepted(true);
    bool draggingChanged = false;
    bool pinchingChanged = false;
    bool testFlick = true;
    int touchPointsCount = event->touchPoints().size();

    if (event->type() == QEvent::TouchBegin) {
        Q_ASSERT(touchPointsCount > 0);
        mViewIface->forceViewActiveFocus();
        if (touchPointsCount > 1 && !mPinching) {
            mPinching = true;
            pinchingChanged = true;
        }
        ResetState();
    } else if (event->type() == QEvent::TouchUpdate) {
        Q_ASSERT(touchPointsCount > 0);
        if (!mDragging) {
            mDragging = true;
            mDragStartY = mContentRect.y() * mContentResolution;
            mMoveDelta = 0;
            draggingChanged = true;
        }

        if (touchPointsCount > 1 && !mPinching) {
            mPinching = true;
            pinchingChanged = true;
        }
    } else if (event->type() == QEvent::TouchEnd) {
        Q_ASSERT(touchPointsCount > 0);
        HandleTouchEnd(draggingChanged, pinchingChanged);
    } else if (event->type() == QEvent::TouchCancel) {
        HandleTouchEnd(draggingChanged, pinchingChanged);
        testFlick = false;
        mCanFlick = false;
    }

    if (testFlick) {
        TestFlickingMode(event);
    }

    qint64 timeStamp = current_timestamp(event);
    MultiTouchInput meventStart(MultiTouchInput::MULTITOUCH_START, timeStamp, TimeStamp(), 0);
    MultiTouchInput meventMove(MultiTouchInput::MULTITOUCH_MOVE, timeStamp, TimeStamp(), 0);
    MultiTouchInput meventEnd(MultiTouchInput::MULTITOUCH_END, timeStamp, TimeStamp(), 0);

    // Add active touch point to cancelled touch sequence.
    if (event->type() == QEvent::TouchCancel && touchPointsCount == 0) {
        QMapIterator<int, QPointF> i(mActiveTouchPoints);
        while (i.hasNext()) {
            i.next();
            QPointF pos = i.value();
            meventEnd.mTouches.AppendElement(SingleTouchData(i.key(),
                                                             mozilla::ScreenIntPoint(pos.x(), pos.y()),
                                                             mozilla::ScreenSize(1, 1),
                                                             180.0f,
                                                             0));
        }
        // All touch point should be cleared but let's clear active touch points anyways.
        mActiveTouchPoints.clear();
    }

    for (int i = 0; i < touchPointsCount; ++i) {
        const QTouchEvent::TouchPoint& pt = event->touchPoints().at(i);
        mozilla::ScreenIntPoint nspt(pt.pos().x(), pt.pos().y());
        switch (pt.state()) {
            case Qt::TouchPointPressed: {
                mActiveTouchPoints.insert(pt.id(), pt.pos());
                meventStart.mTouches.AppendElement(SingleTouchData(pt.id(),
                                                                   nspt,
                                                                   mozilla::ScreenSize(1, 1),
                                                                   180.0f,
                                                                   pt.pressure()));
                break;
            }
            case Qt::TouchPointReleased: {
                mActiveTouchPoints.remove(pt.id());
                meventEnd.mTouches.AppendElement(SingleTouchData(pt.id(),
                                                                 nspt,
                                                                 mozilla::ScreenSize(1, 1),
                                                                 180.0f,
                                                                 pt.pressure()));
                break;
            }
            case Qt::TouchPointMoved:
            case Qt::TouchPointStationary: {
                mActiveTouchPoints.insert(pt.id(), pt.pos());
                meventMove.mTouches.AppendElement(SingleTouchData(pt.id(),
                                                                  nspt,
                                                                  mozilla::ScreenSize(1, 1),
                                                                  180.0f,
                                                                  pt.pressure()));
                break;
            }
            default:
                break;
        }
    }

    if (meventStart.mTouches.Length()) {
        // We should append previous touches to start event in order
        // to make Gecko recognize it as new added touches to existing session
        // and not evict it here http://hg.mozilla.org/mozilla-central/annotate/1d9c510b3742/layout/base/nsPresShell.cpp#l6135
        if (meventMove.mTouches.Length()) {
            meventStart.mTouches.AppendElements(meventMove.mTouches);
        }
        Q_ASSERT(meventStart.mTouches.Length() > 0);
        ReceiveInputEvent(meventStart);
    }
    if (meventMove.mTouches.Length()) {
        /*if (meventStart.mTouches.Length()) {
            meventMove.mTouches.AppendElements(meventStart.mTouches);
        }*/
        Q_ASSERT(meventMove.mTouches.Length() > 0);
        ReceiveInputEvent(meventMove);
    }
    if (meventEnd.mTouches.Length()) {
        Q_ASSERT(meventEnd.mTouches.Length() > 0);
        ReceiveInputEvent(meventEnd);
    }

    if (draggingChanged) {
        mViewIface->draggingChanged();
    }

    if (pinchingChanged) {
        mViewIface->pinchingChanged();
    }

    if (event->type() == QEvent::TouchEnd) {
        if (mCanFlick) {
            UpdateMoving(mCanFlick);
            mViewIface->startMoveMonitoring();
        } else {
            // From dragging (panning) end to clean state
            ResetState();
        }
    } else {
        UpdateMoving(mDragging);
    }
}

void QGraphicsMozViewPrivate::ReceiveInputEvent(const InputData& event)
{
    if (mViewInitialized) {
        mView->ReceiveInputEvent(event);
    }
}
