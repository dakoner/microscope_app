#include "MosaicWidget.h"
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <cmath>

MosaicWidget::MosaicWidget(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

void MosaicWidget::setupScrollbars(QScrollBar *hBar, QScrollBar *vBar)
{
    m_hScrollbar = hBar;
    m_vScrollbar = vBar;
    m_hScrollbar->show();
    m_vScrollbar->show();
    connect(m_hScrollbar, &QScrollBar::valueChanged, this, [this](int value) {
        if (!m_updatingScrollbars) {
            m_panOffset.setX(-value);
            update();
        }
    });
    connect(m_vScrollbar, &QScrollBar::valueChanged, this, [this](int value) {
        if (!m_updatingScrollbars) {
            m_panOffset.setY(-value);
            update();
        }
    });
}

void MosaicWidget::setStageSize(double widthMm, double heightMm)
{
    m_stageWidthMm = widthMm;
    m_stageHeightMm = heightMm;
}

void MosaicWidget::setCalibration(double pxPerMm)
{
    m_calibrationPxPerMm = pxPerMm;
}

void MosaicWidget::setGridSize(int w, int h)
{
    m_gridWidth = w;
    m_gridHeight = h;
    if (!m_batchUpdate)
        update();
}

void MosaicWidget::setCurrentFrameRect(const QRectF &rect)
{
    m_currentFrameRect = rect;
    if (!m_batchUpdate)
        update();
}

void MosaicWidget::setCncPosition(double xMm, double yMm)
{
    if (m_totalWidth > 0 && m_stageWidthMm > 0) {
        double sf = double(m_totalWidth) / (m_stageWidthMm * m_calibrationPxPerMm);
        m_cncImagePos = QPointF(xMm * m_calibrationPxPerMm * sf,
                                (m_stageHeightMm - yMm) * m_calibrationPxPerMm * sf);
    }
}

void MosaicWidget::setOverlayCircles(const QVector<QPair<QPointF, double>> &circles)
{
    m_overlayCircles = circles;
    update();
}

void MosaicWidget::resetMosaic(int width, int height, int tileSize)
{
    m_totalWidth = width;
    m_totalHeight = height;
    m_tileSize = tileSize;
    m_tiles.clear();
    if (width > 0 && height > 0)
        fitToWindow();
    update();
}

void MosaicWidget::updateTile(int row, int col, const QImage &image)
{
    m_tiles[{row, col}] = image;
    if (!m_batchUpdate)
        update();
}

void MosaicWidget::beginUpdate() { m_batchUpdate = true; }
void MosaicWidget::endUpdate() { m_batchUpdate = false; update(); }

QPointF MosaicWidget::widgetToImageCoords(const QPointF &widgetPos) const
{
    if (m_totalWidth == 0) return QPointF();
    return (widgetPos - m_panOffset) / m_zoomFactor;
}

QPointF MosaicWidget::imageToWidgetCoords(const QPointF &imagePos) const
{
    if (m_totalWidth == 0) return QPointF();
    return (imagePos * m_zoomFactor) + m_panOffset;
}

void MosaicWidget::fitToWindow()
{
    if (m_totalWidth == 0 || m_totalHeight == 0) return;

    double scaleW = double(rect().width()) / m_totalWidth;
    double scaleH = double(rect().height()) / m_totalHeight;
    m_zoomFactor = std::min(scaleW, scaleH);

    double drawnW = m_totalWidth * m_zoomFactor;
    double drawnH = m_totalHeight * m_zoomFactor;
    m_panOffset = QPointF((rect().width() - drawnW) / 2.0,
                          (rect().height() - drawnH) / 2.0);
    clampPanOffset();
    updateScrollbars();
    update();
}

void MosaicWidget::clampPanOffset()
{
    double viewW = rect().width();
    double viewH = rect().height();
    double drawnW = m_totalWidth * m_zoomFactor;
    double drawnH = m_totalHeight * m_zoomFactor;

    double x, y;
    if (drawnW > viewW)
        x = qBound(-(drawnW - viewW), m_panOffset.x(), 0.0);
    else
        x = (viewW - drawnW) / 2.0;

    if (drawnH > viewH)
        y = qBound(-(drawnH - viewH), m_panOffset.y(), 0.0);
    else
        y = (viewH - drawnH) / 2.0;

    m_panOffset = QPointF(x, y);
}

void MosaicWidget::updateScrollbars()
{
    if (!m_hScrollbar || !m_vScrollbar) return;

    double viewW = rect().width();
    double viewH = rect().height();
    double drawnW = m_totalWidth * m_zoomFactor;
    double drawnH = m_totalHeight * m_zoomFactor;

    m_updatingScrollbars = true;

    if (drawnW > viewW) {
        m_hScrollbar->setRange(0, int(drawnW - viewW));
        m_hScrollbar->setPageStep(int(viewW));
        m_hScrollbar->setValue(int(-m_panOffset.x()));
        m_hScrollbar->setEnabled(true);
    } else {
        m_hScrollbar->setRange(0, 0);
        m_hScrollbar->setEnabled(false);
    }

    if (drawnH > viewH) {
        m_vScrollbar->setRange(0, int(drawnH - viewH));
        m_vScrollbar->setPageStep(int(viewH));
        m_vScrollbar->setValue(int(-m_panOffset.y()));
        m_vScrollbar->setEnabled(true);
    } else {
        m_vScrollbar->setRange(0, 0);
        m_vScrollbar->setEnabled(false);
    }

    m_updatingScrollbars = false;
}

void MosaicWidget::doZoom(int direction, QPointF center)
{
    if (m_totalWidth == 0) return;

    double increment = 0.1 * direction;
    double oldZoom = m_zoomFactor;
    m_zoomFactor = qBound(0.1, m_zoomFactor + increment, 10.0);

    if (center.isNull()) {
        // Zoom centered on current CNC/stage position
        if (!m_cncImagePos.isNull())
            center = imageToWidgetCoords(m_cncImagePos);
        else
            center = QPointF(rect().width() / 2.0, rect().height() / 2.0);
    }

    QPointF imgBefore = (center - m_panOffset) / oldZoom;
    QPointF widgetAfter = imgBefore * m_zoomFactor;
    m_panOffset = center - widgetAfter;

    clampPanOffset();
    updateScrollbars();
    update();
}

void MosaicWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    clampPanOffset();
    updateScrollbars();
}

void MosaicWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    fitToWindow();
}

void MosaicWidget::keyPressEvent(QKeyEvent *event)
{
    if (m_totalWidth == 0) {
        QWidget::keyPressEvent(event);
        return;
    }
    if (event->key() == Qt::Key_PageUp) {
        doZoom(1);
        event->accept();
    } else if (event->key() == Qt::Key_PageDown) {
        doZoom(-1);
        event->accept();
    } else {
        QWidget::keyPressEvent(event);
    }
}

void MosaicWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor(20, 20, 20));

    if (m_totalWidth > 0 && m_totalHeight > 0) {
        painter.save();
        painter.translate(m_panOffset);
        painter.scale(m_zoomFactor, m_zoomFactor);

        // Visible region in image coordinates for tile culling
        QRectF visibleImage(-m_panOffset.x() / m_zoomFactor,
                            -m_panOffset.y() / m_zoomFactor,
                            width() / m_zoomFactor,
                            height() / m_zoomFactor);

        for (auto it = m_tiles.constBegin(); it != m_tiles.constEnd(); ++it) {
            int row = it.key().first;
            int col = it.key().second;
            int x = col * m_tileSize;
            int y = row * m_tileSize;
            QRectF tileRect(x, y, it.value().width(), it.value().height());
            if (!tileRect.intersects(visibleImage))
                continue;
            painter.drawImage(x, y, it.value());
        }

        // Selected rects (pink)
        for (const auto &r : m_selectedRects) {
            QPen pen(QColor(255, 105, 180), 3.0 / m_zoomFactor);
            painter.setPen(pen);
            painter.setBrush(QColor(255, 105, 180, 50));
            painter.drawRect(r);
        }

        // Current frame rect (green)
        if (!m_currentFrameRect.isNull()) {
            QPen pen(Qt::green, 2.0 / m_zoomFactor);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(m_currentFrameRect);
        }

        // Overlay circles
        for (const auto &[center, radius] : m_overlayCircles) {
            QPen pen(QColor(255, 196, 0), 3.0 / m_zoomFactor);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(center, radius, radius);
        }

        // Millimetre grid (always visible)
        if (m_stageWidthMm > 0 && m_calibrationPxPerMm > 0) {
            double scaleFactor = double(m_totalWidth) / (m_stageWidthMm * m_calibrationPxPerMm);
            double pxPerMm = m_calibrationPxPerMm * scaleFactor;
            // Pick a nice mm spacing so we get ~10-40 lines
            double rawStep = m_stageWidthMm / 20.0;
            double nice = 1.0;
            if (rawStep >= 50) nice = 50;
            else if (rawStep >= 20) nice = 20;
            else if (rawStep >= 10) nice = 10;
            else if (rawStep >= 5) nice = 5;
            else if (rawStep >= 2) nice = 2;
            double stepPx = nice * pxPerMm;
            if (stepPx > 1) {
                QPen pen(QColor(100, 100, 100, 80), 1.0 / m_zoomFactor);
                painter.setPen(pen);
                for (double x = stepPx; x < m_totalWidth; x += stepPx)
                    painter.drawLine(QPointF(x, 0), QPointF(x, m_totalHeight));
                for (double y = stepPx; y < m_totalHeight; y += stepPx)
                    painter.drawLine(QPointF(0, y), QPointF(m_totalWidth, y));
            }
        }

        // Camera FOV grid
        if (m_gridWidth > 0 && m_gridHeight > 0) {
            QPen pen(QColor(128, 128, 128, 100), 1.0 / m_zoomFactor);
            painter.setPen(pen);
            int maxLines = 2000;
            int numV = m_totalWidth / m_gridWidth;
            if (numV < maxLines) {
                for (int i = 1; i <= numV; ++i)
                    painter.drawLine(i * m_gridWidth, 0, i * m_gridWidth, m_totalHeight);
            }
            int numH = m_totalHeight / m_gridHeight;
            if (numH < maxLines) {
                for (int i = 1; i <= numH; ++i)
                    painter.drawLine(0, i * m_gridHeight, m_totalWidth, i * m_gridHeight);
            }
        }

        painter.restore();
    }

    drawAxes(painter);

    // Selection rectangle while dragging
    if (m_isSelecting && !m_selStartPos.isNull() && !m_selCurrentPos.isNull()) {
        QPen pen(QColor(0, 255, 255, 200), 2, Qt::DashLine);
        painter.setPen(pen);
        painter.setBrush(QColor(0, 100, 100, 50));
        QRectF r(m_selStartPos, m_selCurrentPos);
        painter.drawRect(r.normalized());
    }
}

void MosaicWidget::drawAxes(QPainter &painter)
{
    if (m_totalWidth == 0 || m_stageWidthMm == 0) return;

    int margin = 40;
    QRect axisRect = rect().adjusted(margin, margin, -margin, -margin);

    painter.setPen(QPen(QColor(220, 220, 220), 1));

    // Bottom axis labels
    int numLabels = 11;
    for (int i = 0; i <= numLabels; ++i) {
        double valMm = double(i) / numLabels * m_stageWidthMm;
        double imgX = valMm * m_calibrationPxPerMm;
        double widgetX = imageToWidgetCoords(QPointF(imgX, 0)).x();
        if (widgetX >= axisRect.left() - 20 && widgetX <= axisRect.right() + 20) {
            painter.drawText(QRectF(widgetX - 20, axisRect.bottom() + 5, 40, 20),
                             Qt::AlignCenter, QString::number(valMm, 'f', 1));
        }
    }

    // Left axis labels
    for (int i = 0; i <= numLabels; ++i) {
        double valMm = double(i) / numLabels * m_stageHeightMm;
        double imgY = m_totalHeight - (valMm * m_calibrationPxPerMm);
        double widgetY = imageToWidgetCoords(QPointF(0, imgY)).y();
        if (widgetY >= axisRect.top() - 10 && widgetY <= axisRect.bottom() + 10) {
            painter.drawText(QRectF(axisRect.left() - margin, widgetY - 10, margin - 5, 20),
                             Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(valMm, 'f', 1));
        }
    }
}

void MosaicWidget::mousePressEvent(QMouseEvent *event)
{
    if (m_totalWidth == 0) return;
    m_lastMousePos = event->position();

    if (event->button() == Qt::LeftButton && (event->modifiers() & Qt::ShiftModifier)) {
        m_isSelecting = true;
        m_selStartPos = event->position();
        m_selCurrentPos = event->position();
    } else if (event->button() == Qt::LeftButton) {
        m_isPanning = true;
        setCursor(Qt::ClosedHandCursor);
    } else if (event->button() == Qt::RightButton) {
        m_selectedRects.clear();
        emit selectionsChanged(m_selectedRects);
        update();
    }
}

void MosaicWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (m_totalWidth == 0) return;

    QPointF imgCoords = widgetToImageCoords(event->position());
    emit mouseMoved(imgCoords.x(), imgCoords.y());

    QPointF delta = event->position() - m_lastMousePos;
    if (m_isPanning) {
        m_panOffset += delta;
        clampPanOffset();
        updateScrollbars();
        update();
    } else if (m_isSelecting) {
        m_selCurrentPos = event->position();
        update();
    }
    m_lastMousePos = event->position();
}

void MosaicWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_totalWidth == 0) return;

    if (m_isSelecting) {
        double dist = (event->position() - m_selStartPos).manhattanLength();
        if (dist < 5) {
            QPointF imgCoords = widgetToImageCoords(event->position());
            emit clicked(imgCoords.x(), imgCoords.y());
        } else {
            QPointF startImg = widgetToImageCoords(m_selStartPos);
            QPointF endImg = widgetToImageCoords(event->position());
            QRectF selRect(startImg, endImg);
            selRect = selRect.normalized();
            QRectF imgBounds(0, 0, m_totalWidth, m_totalHeight);
            selRect = selRect.intersected(imgBounds);
            if (!selRect.isEmpty()) {
                m_selectedRects.append(selRect);
                emit selectionsChanged(m_selectedRects);
                emit selectionMade(selRect.x(), selRect.y(),
                                   selRect.width(), selRect.height());
            }
        }
        m_selStartPos = QPointF();
        m_selCurrentPos = QPointF();
        update();
    }

    m_isPanning = false;
    m_isSelecting = false;
    setCursor(Qt::ArrowCursor);
}
