#include "MosaicWidget.h"
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QWheelEvent>
#include <algorithm>
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
                                yMm * m_calibrationPxPerMm * sf);
    }
}

void MosaicWidget::setOverlayCircles(const QVector<QPair<QPointF, double>> &circles)
{
    m_overlayCircles = circles;
    update();
}

void MosaicWidget::setWaypoints(const QVector<MosaicWaypointMarker> &waypoints)
{
    m_waypoints = waypoints;
    update();
}

void MosaicWidget::setSelections(const QVector<QRectF> &imageRects)
{
    const QRectF imageBounds(0, 0, m_totalWidth, m_totalHeight);
    QVector<QRectF> clipped;
    for (const QRectF &rect : imageRects) {
        const QRectF selection = rect.normalized().intersected(imageBounds);
        if (!selection.isEmpty())
            clipped.append(selection);
    }

    m_selectedRects = clipped;
    m_activeRectIndex = m_selectedRects.isEmpty() ? -1 : (m_selectedRects.size() - 1);
    m_isSelecting = false;
    m_isEditingRect = false;
    m_editHandle = RectHandle::None;
    m_selStartPos = QPointF();
    m_selCurrentPos = QPointF();
    emit selectionsChanged(m_selectedRects);
    update();
}

bool MosaicWidget::removeActiveSelection()
{
    if (m_activeRectIndex < 0 || m_activeRectIndex >= m_selectedRects.size()) {
        return false;
    }
    m_selectedRects.removeAt(m_activeRectIndex);
    m_activeRectIndex = m_selectedRects.isEmpty()
        ? -1
        : std::min(m_activeRectIndex, static_cast<int>(m_selectedRects.size()) - 1);
    m_isEditingRect = false;
    m_editHandle = RectHandle::None;
    emit selectionsChanged(m_selectedRects);
    update();
    return true;
}

void MosaicWidget::resetMosaic(int width, int height, int tileSize)
{
    m_totalWidth = width;
    m_totalHeight = height;
    m_tileSize = tileSize;
    m_tiles.clear();
    m_hasInitializedView = false;
    if (width > 0 && height > 0 && !rect().isEmpty()) {
        fitToWindow();
        m_hasInitializedView = true;
    }
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
    if (!m_hasInitializedView && m_totalWidth > 0 && m_totalHeight > 0 && !rect().isEmpty()) {
        fitToWindow();
        m_hasInitializedView = true;
        return;
    }
    clampPanOffset();
    updateScrollbars();
}

void MosaicWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (!m_hasInitializedView && m_totalWidth > 0 && m_totalHeight > 0 && !rect().isEmpty()) {
        fitToWindow();
        m_hasInitializedView = true;
    }
}

QPixmap MosaicWidget::createViewportPreview(const QSize &size) const
{
    if (!size.isValid() || m_totalWidth <= 0 || m_totalHeight <= 0)
        return {};

    QRectF visibleImage(-m_panOffset.x() / m_zoomFactor,
                        -m_panOffset.y() / m_zoomFactor,
                        width() / m_zoomFactor,
                        height() / m_zoomFactor);
    visibleImage = visibleImage.intersected(QRectF(0, 0, m_totalWidth, m_totalHeight));
    if (visibleImage.isEmpty())
        visibleImage = QRectF(0, 0, m_totalWidth, m_totalHeight);

    QPixmap preview(size);
    preview.fill(QColor(20, 20, 20));

    QPainter painter(&preview);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    double scaleW = double(size.width()) / visibleImage.width();
    double scaleH = double(size.height()) / visibleImage.height();
    double scale = std::min(scaleW, scaleH);
    if (scale <= 0.0)
        return preview;

    double drawnW = visibleImage.width() * scale;
    double drawnH = visibleImage.height() * scale;
    QPointF offset((size.width() - drawnW) / 2.0, (size.height() - drawnH) / 2.0);

    painter.translate(offset - QPointF(visibleImage.x() * scale, visibleImage.y() * scale));
    painter.scale(scale, scale);
    painter.fillRect(QRectF(0, 0, m_totalWidth, m_totalHeight), Qt::white);

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

    for (const auto &r : m_selectedRects) {
        QPen pen(QColor(255, 105, 180), 3.0 / scale);
        painter.setPen(pen);
        painter.setBrush(QColor(255, 105, 180, 50));
        painter.drawRect(r);
    }

    if (!m_currentFrameRect.isNull()) {
        QPen pen(Qt::green, 2.0 / scale);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(m_currentFrameRect);
    }

    for (const auto &[center, radius] : m_overlayCircles) {
        QPen pen(QColor(255, 196, 0), 3.0 / scale);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(center, radius, radius);
    }

    for (const auto &wp : m_waypoints) {
        const double r = 4.0 / scale;
        painter.setPen(QPen(QColor(10, 25, 32), 1.5 / scale));
        painter.setBrush(QColor(0, 229, 255));
        painter.drawEllipse(wp.imagePos, r, r);
    }

    if (m_gridWidth > 0 && m_gridHeight > 0) {
        QPen pen(QColor(128, 128, 128, 100), 1.0 / scale);
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

    return preview;
}

void MosaicWidget::keyPressEvent(QKeyEvent *event)
{
    if (m_totalWidth == 0) {
        QWidget::keyPressEvent(event);
        return;
    }
    if (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab) {
        emit toggleViewRequested();
        event->accept();
    } else if (event->key() == Qt::Key_PageUp) {
        doZoom(1);
        event->accept();
    } else if (event->key() == Qt::Key_PageDown) {
        doZoom(-1);
        event->accept();
    } else {
        QWidget::keyPressEvent(event);
    }
}

void MosaicWidget::wheelEvent(QWheelEvent *event)
{
    if (m_totalWidth == 0) {
        QWidget::wheelEvent(event);
        return;
    }

    int deltaY = event->angleDelta().y();
    if (deltaY == 0) {
        QWidget::wheelEvent(event);
        return;
    }

    int direction = (deltaY > 0) ? 1 : -1;
    int steps = std::max(1, std::abs(deltaY) / 120);
    for (int i = 0; i < steps; ++i) {
        doZoom(direction, event->position());
    }

    event->accept();
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

        // Selected rects (pink; active selection slightly brighter)
        for (int i = 0; i < m_selectedRects.size(); ++i) {
            const QRectF &r = m_selectedRects.at(i);
            const bool isActive = (i == m_activeRectIndex);
            QPen pen(isActive ? QColor(255, 130, 200) : QColor(255, 105, 180),
                     (isActive ? 3.0 : 2.0) / m_zoomFactor);
            painter.setPen(pen);
            painter.setBrush(isActive ? QColor(255, 105, 180, 65) : QColor(255, 105, 180, 40));
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

        drawSelectionHandles(painter);

        // Waypoint markers drawn in widget coordinates so pins and labels stay crisp at any zoom.
        if (!m_waypoints.isEmpty()) {
            painter.save();
            QFont labelFont = painter.font();
            labelFont.setPointSize(8);
            labelFont.setBold(true);
            painter.setFont(labelFont);
            const QFontMetrics fm(labelFont);
            const QRect paddedBounds = rect().adjusted(-60, -60, 60, 60);

            for (const auto &wp : m_waypoints) {
                const QPointF p = imageToWidgetCoords(wp.imagePos);
                if (!paddedBounds.contains(p.toPoint()))
                    continue;

                // Crosshair + cyan pin
                painter.setPen(QPen(QColor(0, 229, 255, 220), 1.5));
                painter.drawLine(QPointF(p.x() - 8, p.y()), QPointF(p.x() + 8, p.y()));
                painter.drawLine(QPointF(p.x(), p.y() - 8), QPointF(p.x(), p.y() + 8));

                painter.setPen(QPen(QColor(10, 25, 32), 1.5));
                painter.setBrush(QColor(0, 229, 255));
                painter.drawEllipse(p, 4.5, 4.5);

                if (!wp.name.isEmpty()) {
                    const QString label = wp.name;
                    const int textW = fm.horizontalAdvance(label);
                    const int textH = fm.height();
                    const QRectF tagRect(p.x() + 9, p.y() - textH / 2.0 - 2.0,
                                         textW + 8, textH + 4);
                    painter.setPen(QPen(QColor(0, 229, 255, 180), 1));
                    painter.setBrush(QColor(15, 23, 30, 200));
                    painter.drawRoundedRect(tagRect, 3, 3);
                    painter.setPen(Qt::white);
                    painter.drawText(tagRect, Qt::AlignCenter, label);
                }
            }
            painter.restore();
        }
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

void MosaicWidget::drawSelectionHandles(QPainter &painter)
{
    if (m_selectedRects.isEmpty() || m_totalWidth <= 0)
        return;

    painter.save();
    QFont labelFont = painter.font();
    labelFont.setPointSize(8);
    labelFont.setBold(true);
    painter.setFont(labelFont);
    const QFontMetrics fm(labelFont);

    const double pxPerMm = (m_stageWidthMm > 0 && m_calibrationPxPerMm > 0)
        ? (double(m_totalWidth) / m_stageWidthMm)
        : 0.0;

    for (int i = 0; i < m_selectedRects.size(); ++i) {
        const QRectF &imgRect = m_selectedRects.at(i);
        const QPointF tl = imageToWidgetCoords(imgRect.topLeft());
        const QPointF br = imageToWidgetCoords(imgRect.bottomRight());
        const QRectF wr(tl, br);
        const bool isActive = (i == m_activeRectIndex);

        // Draw 8 resize handles (4 corners + 4 edge midpoints) on every region,
        // highlighted on the active region so editing affordances are obvious.
        const double half = isActive ? 4.5 : 3.5;
        const QPointF midTop((wr.left() + wr.right()) / 2.0, wr.top());
        const QPointF midBot((wr.left() + wr.right()) / 2.0, wr.bottom());
        const QPointF midLeft(wr.left(), (wr.top() + wr.bottom()) / 2.0);
        const QPointF midRight(wr.right(), (wr.top() + wr.bottom()) / 2.0);

        const QPointF handles[8] = {
            wr.topLeft(), wr.topRight(), wr.bottomLeft(), wr.bottomRight(),
            midTop, midBot, midLeft, midRight
        };

        painter.setPen(QPen(QColor(30, 15, 25), 1.5));
        painter.setBrush(isActive ? QColor(255, 255, 255) : QColor(255, 180, 220));
        for (const QPointF &h : handles) {
            painter.drawRect(QRectF(h.x() - half, h.y() - half, half * 2.0, half * 2.0));
        }

        // Dimension badge on the active rectangle (or when editing)
        if (isActive && pxPerMm > 0.0) {
            const double x1 = imgRect.left() / pxPerMm;
            const double y1 = imgRect.top() / pxPerMm;
            const double wMm = imgRect.width() / pxPerMm;
            const double hMm = imgRect.height() / pxPerMm;
            const QString badge = QString("%1 x %2 mm  (%3, %4)")
                                      .arg(wMm, 0, 'f', 2)
                                      .arg(hMm, 0, 'f', 2)
                                      .arg(x1, 0, 'f', 1)
                                      .arg(y1, 0, 'f', 1);
            const int tw = fm.horizontalAdvance(badge) + 10;
            const int th = fm.height() + 4;
            QPointF badgePos(wr.left(), wr.top() - th - 6.0);
            if (badgePos.y() < 4.0) {
                badgePos.setY(wr.bottom() + 6.0);
            }
            const QRectF badgeRect(badgePos.x(), badgePos.y(), tw, th);
            painter.setPen(QPen(QColor(255, 130, 200, 220), 1));
            painter.setBrush(QColor(25, 15, 22, 210));
            painter.drawRoundedRect(badgeRect, 3, 3);
            painter.setPen(Qt::white);
            painter.drawText(badgeRect, Qt::AlignCenter, badge);
        }
    }
    painter.restore();
}

MosaicWidget::RectHit MosaicWidget::hitTestSelection(const QPointF &widgetPos) const
{
    if (m_selectedRects.isEmpty() || m_totalWidth <= 0) {
        return {};
    }

    constexpr double kCornerRadius = 9.0;
    constexpr double kEdgeTol = 7.0;

    // Test active rect first, then remaining rects top-to-bottom.
    QVector<int> order;
    order.reserve(m_selectedRects.size());
    if (m_activeRectIndex >= 0 && m_activeRectIndex < m_selectedRects.size()) {
        order.append(m_activeRectIndex);
    }
    for (int i = m_selectedRects.size() - 1; i >= 0; --i) {
        if (i != m_activeRectIndex) {
            order.append(i);
        }
    }

    auto nearPt = [](const QPointF &a, const QPointF &b, double r) {
        return std::abs(a.x() - b.x()) <= r && std::abs(a.y() - b.y()) <= r;
    };

    for (int idx : order) {
        const QRectF &imgRect = m_selectedRects.at(idx);
        const QRectF wr(imageToWidgetCoords(imgRect.topLeft()),
                        imageToWidgetCoords(imgRect.bottomRight()));

        // Corners first
        if (nearPt(widgetPos, wr.topLeft(), kCornerRadius))
            return {idx, RectHandle::TopLeft};
        if (nearPt(widgetPos, wr.topRight(), kCornerRadius))
            return {idx, RectHandle::TopRight};
        if (nearPt(widgetPos, wr.bottomLeft(), kCornerRadius))
            return {idx, RectHandle::BottomLeft};
        if (nearPt(widgetPos, wr.bottomRight(), kCornerRadius))
            return {idx, RectHandle::BottomRight};

        // Edges next (grab anywhere along an edge)
        const bool inY = (widgetPos.y() >= wr.top() - kEdgeTol && widgetPos.y() <= wr.bottom() + kEdgeTol);
        const bool inX = (widgetPos.x() >= wr.left() - kEdgeTol && widgetPos.x() <= wr.right() + kEdgeTol);
        if (inY && std::abs(widgetPos.x() - wr.left()) <= kEdgeTol)
            return {idx, RectHandle::Left};
        if (inY && std::abs(widgetPos.x() - wr.right()) <= kEdgeTol)
            return {idx, RectHandle::Right};
        if (inX && std::abs(widgetPos.y() - wr.top()) <= kEdgeTol)
            return {idx, RectHandle::Top};
        if (inX && std::abs(widgetPos.y() - wr.bottom()) <= kEdgeTol)
            return {idx, RectHandle::Bottom};

        // Interior = move
        if (wr.contains(widgetPos))
            return {idx, RectHandle::Move};
    }

    return {};
}

QString MosaicWidget::hitTestWaypoint(const QPointF &widgetPos) const
{
    if (m_waypoints.isEmpty() || m_totalWidth <= 0) {
        return {};
    }

    QFont labelFont = font();
    labelFont.setPointSize(8);
    labelFont.setBold(true);
    const QFontMetrics fm(labelFont);

    constexpr double kPinRadius = 12.0;
    for (int i = m_waypoints.size() - 1; i >= 0; --i) {
        const auto &wp = m_waypoints.at(i);
        const QPointF p = imageToWidgetCoords(wp.imagePos);
        if (std::hypot(widgetPos.x() - p.x(), widgetPos.y() - p.y()) <= kPinRadius) {
            return wp.id;
        }
        if (!wp.name.isEmpty()) {
            const int textW = fm.horizontalAdvance(wp.name);
            const int textH = fm.height();
            const QRectF tagRect(p.x() + 9, p.y() - textH / 2.0 - 2.0,
                                 textW + 8, textH + 4);
            if (tagRect.adjusted(-3, -3, 3, 3).contains(widgetPos)) {
                return wp.id;
            }
        }
    }
    return {};
}

Qt::CursorShape MosaicWidget::cursorForHandle(RectHandle handle)
{
    switch (handle) {
    case RectHandle::TopLeft:
    case RectHandle::BottomRight: return Qt::SizeFDiagCursor;
    case RectHandle::TopRight:
    case RectHandle::BottomLeft:  return Qt::SizeBDiagCursor;
    case RectHandle::Left:
    case RectHandle::Right:       return Qt::SizeHorCursor;
    case RectHandle::Top:
    case RectHandle::Bottom:      return Qt::SizeVerCursor;
    case RectHandle::Move:        return Qt::SizeAllCursor;
    case RectHandle::None:
    default:                      return Qt::ArrowCursor;
    }
}

void MosaicWidget::mousePressEvent(QMouseEvent *event)
{
    if (m_totalWidth == 0) return;
    m_lastMousePos = event->position();

    if (event->button() == Qt::LeftButton && (event->modifiers() & Qt::ControlModifier)) {
        const QString wpId = hitTestWaypoint(event->position());
        if (!wpId.isEmpty()) {
            emit waypointDeleteRequested(wpId);
            return;
        }
    }

    if (event->button() == Qt::LeftButton && (event->modifiers() & Qt::ShiftModifier)) {
        m_isSelecting = true;
        m_selStartPos = event->position();
        m_selCurrentPos = event->position();
    } else if (event->button() == Qt::LeftButton) {
        const RectHit hit = hitTestSelection(event->position());
        if (hit.index >= 0 && hit.handle != RectHandle::None) {
            m_activeRectIndex = hit.index;
            m_isEditingRect = true;
            m_editHandle = hit.handle;
            m_editStartImgPos = widgetToImageCoords(event->position());
            m_editOriginalRect = m_selectedRects.at(hit.index);
            setCursor(cursorForHandle(hit.handle));
            update();
        } else {
            m_isPanning = true;
            setCursor(Qt::ClosedHandCursor);
        }
    } else if (event->button() == Qt::RightButton) {
        const RectHit hit = hitTestSelection(event->position());
        if (hit.index >= 0) {
            // Right-clicking a specific region removes just that region.
            m_selectedRects.removeAt(hit.index);
            m_activeRectIndex = m_selectedRects.isEmpty()
                ? -1
                : std::min(hit.index, static_cast<int>(m_selectedRects.size()) - 1);
        } else {
            // Right-clicking empty space clears all regions.
            m_selectedRects.clear();
            m_activeRectIndex = -1;
        }
        m_isEditingRect = false;
        m_editHandle = RectHandle::None;
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
    if (m_isEditingRect && m_activeRectIndex >= 0 && m_activeRectIndex < m_selectedRects.size()) {
        const double maxW = static_cast<double>(m_totalWidth);
        const double maxH = static_cast<double>(m_totalHeight);
        // Minimum edited size (~0.05 mm or 4 mosaic px) so a region cannot collapse to empty.
        const double minDim = std::max(4.0, (m_stageWidthMm > 0 ? (maxW / m_stageWidthMm) * 0.05 : 4.0));
        QRectF r = m_editOriginalRect;
        const QPointF imgDelta = imgCoords - m_editStartImgPos;

        if (m_editHandle == RectHandle::Move) {
            const double nx = std::clamp(r.x() + imgDelta.x(), 0.0, std::max(0.0, maxW - r.width()));
            const double ny = std::clamp(r.y() + imgDelta.y(), 0.0, std::max(0.0, maxH - r.height()));
            r.moveTopLeft(QPointF(nx, ny));
        } else {
            double left = r.left();
            double right = r.right();
            double top = r.top();
            double bottom = r.bottom();

            if (m_editHandle == RectHandle::Left ||
                m_editHandle == RectHandle::TopLeft ||
                m_editHandle == RectHandle::BottomLeft) {
                left = std::clamp(r.left() + imgDelta.x(), 0.0, right - minDim);
            }
            if (m_editHandle == RectHandle::Right ||
                m_editHandle == RectHandle::TopRight ||
                m_editHandle == RectHandle::BottomRight) {
                right = std::clamp(r.right() + imgDelta.x(), left + minDim, maxW);
            }
            if (m_editHandle == RectHandle::Top ||
                m_editHandle == RectHandle::TopLeft ||
                m_editHandle == RectHandle::TopRight) {
                top = std::clamp(r.top() + imgDelta.y(), 0.0, bottom - minDim);
            }
            if (m_editHandle == RectHandle::Bottom ||
                m_editHandle == RectHandle::BottomLeft ||
                m_editHandle == RectHandle::BottomRight) {
                bottom = std::clamp(r.bottom() + imgDelta.y(), top + minDim, maxH);
            }
            r = QRectF(QPointF(left, top), QPointF(right, bottom));
        }

        m_selectedRects[m_activeRectIndex] = r;
        update();
    } else if (m_isPanning) {
        m_panOffset += delta;
        clampPanOffset();
        updateScrollbars();
        update();
    } else if (m_isSelecting) {
        m_selCurrentPos = event->position();
        update();
    } else {
        if ((event->modifiers() & Qt::ControlModifier) &&
            !hitTestWaypoint(event->position()).isEmpty()) {
            setCursor(Qt::PointingHandCursor);
        } else if (event->modifiers() & Qt::ShiftModifier) {
            setCursor(Qt::CrossCursor);
        } else {
            const RectHit hit = hitTestSelection(event->position());
            setCursor(cursorForHandle(hit.handle));
        }
    }
    m_lastMousePos = event->position();
}

void MosaicWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_totalWidth == 0) return;

    if (m_isEditingRect) {
        m_isEditingRect = false;
        m_editHandle = RectHandle::None;
        if (m_activeRectIndex >= 0 && m_activeRectIndex < m_selectedRects.size() &&
            m_selectedRects.at(m_activeRectIndex) != m_editOriginalRect) {
            emit selectionsChanged(m_selectedRects);
        }
        update();
    } else if (m_isSelecting) {
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
                m_activeRectIndex = m_selectedRects.size() - 1;
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
    const RectHit hit = hitTestSelection(event->position());
    setCursor(cursorForHandle(hit.handle));
}
