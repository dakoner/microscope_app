#include "MosaicPanel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QPixmap>
#include <cmath>
#include <algorithm>

MosaicPanel::MosaicPanel(double stageWidthMm, double stageHeightMm,
                         double rulerCalibrationPxPerMm, QWidget *parent)
    : QWidget(parent)
    , m_stageWidthMm(stageWidthMm)
    , m_stageHeightMm(stageHeightMm)
    , m_calibrationPxPerMm(rulerCalibrationPxPerMm)
{
    m_mosaicWidthPx = int(m_stageWidthMm * m_calibrationPxPerMm * SCALE_FACTOR);
    m_mosaicHeightPx = int(m_stageHeightMm * m_calibrationPxPerMm * SCALE_FACTOR);

    if (m_calibrationPxPerMm <= 0 || m_mosaicWidthPx <= 0 || m_mosaicHeightPx <= 0) {
        auto *layout = new QVBoxLayout(this);
        layout->addWidget(new QLabel("Error: Invalid Calibration"));
        return;
    }

    m_cols = (m_mosaicWidthPx + TILE_SIZE - 1) / TILE_SIZE;
    m_rows = (m_mosaicHeightPx + TILE_SIZE - 1) / TILE_SIZE;

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    m_displayWidget = new MosaicWidget;
    m_positionLabel = new QLabel("CNC: 0.0 mm, 0.0 mm");
    m_cursorLabel = new QLabel;

    auto *hScrollbar = new QScrollBar(Qt::Horizontal);
    auto *vScrollbar = new QScrollBar(Qt::Vertical);

    // Layout: display widget + vertical scrollbar side by side, h scrollbar at bottom
    auto *bodyLayout = new QHBoxLayout;
    bodyLayout->setSpacing(0);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->addWidget(m_displayWidget, 1);
    bodyLayout->addWidget(vScrollbar);

    mainLayout->addLayout(bodyLayout, 1);
    mainLayout->addWidget(hScrollbar);
    mainLayout->addWidget(m_positionLabel);
    mainLayout->addWidget(m_cursorLabel);

    m_displayWidget->setupScrollbars(hScrollbar, vScrollbar);
    m_displayWidget->setStageSize(m_stageWidthMm, m_stageHeightMm);
    m_displayWidget->setCalibration(m_calibrationPxPerMm);
    m_displayWidget->resetMosaic(m_mosaicWidthPx, m_mosaicHeightPx, TILE_SIZE);

    // Tiles are allocated lazily in updateMosaic() when the camera first writes to them.

    // Draw a circle at the center of the stage
    double centerX = m_mosaicWidthPx / 2.0;
    double centerY = m_mosaicHeightPx / 2.0;
    double radius = std::min(m_mosaicWidthPx, m_mosaicHeightPx) / 2.0;
    m_displayWidget->setOverlayCircles({{QPointF(centerX, centerY), radius}});

    connect(m_displayWidget, &MosaicWidget::clicked, this, &MosaicPanel::onMosaicClicked);
    connect(m_displayWidget, &MosaicWidget::selectionsChanged, this, &MosaicPanel::onSelectionsChanged);
    connect(m_displayWidget, &MosaicWidget::mouseMoved, this, &MosaicPanel::onMosaicMouseMoved);
}

void MosaicPanel::setCncPosition(double xMm, double yMm)
{
    if (m_displayWidget)
        m_displayWidget->setCncPosition(xMm, yMm);
}

void MosaicPanel::setStageCircles(const QVector<std::tuple<double, double, double>> &circlesMm)
{
    QVector<QPair<QPointF, double>> px;
    for (const auto &[cx, cy, radius] : circlesMm) {
        double pxX = cx * m_calibrationPxPerMm * SCALE_FACTOR;
        double pxY = (m_stageHeightMm - cy) * m_calibrationPxPerMm * SCALE_FACTOR;
        double pxR = radius * m_calibrationPxPerMm * SCALE_FACTOR;
        px.append({QPointF(pxX, pxY), pxR});
    }
    m_displayWidget->setOverlayCircles(px);
}

void MosaicPanel::updateMosaic(const QImage &cameraFrame, double cncXMm, double cncYMm)
{
    if (m_calibrationPxPerMm <= 0 || cameraFrame.isNull()) return;

    if (m_cameraFrameWidthPx != cameraFrame.width() ||
        m_cameraFrameHeightPx != cameraFrame.height()) {
        m_cameraFrameWidthPx = cameraFrame.width();
        m_cameraFrameHeightPx = cameraFrame.height();
        int scaledGridW = int(m_cameraFrameWidthPx * SCALE_FACTOR);
        int scaledGridH = int(m_cameraFrameHeightPx * SCALE_FACTOR);
        m_displayWidget->setGridSize(scaledGridW, scaledGridH);
    }

    double fovWidthMm = m_cameraFrameWidthPx / m_calibrationPxPerMm;
    double fovHeightMm = m_cameraFrameHeightPx / m_calibrationPxPerMm;

    double tlXmm = cncXMm - fovWidthMm / 2.0;
    double tlYmm = cncYMm + fovHeightMm / 2.0;

    int drawX = int(tlXmm * m_calibrationPxPerMm * SCALE_FACTOR);
    int drawY = int((m_stageHeightMm - tlYmm) * m_calibrationPxPerMm * SCALE_FACTOR);

    int scaledW = int(m_cameraFrameWidthPx * SCALE_FACTOR);
    int scaledH = int(m_cameraFrameHeightPx * SCALE_FACTOR);
    QRect frameRect(drawX, drawY, scaledW, scaledH);
    m_currentFrameRect = QRectF(frameRect);

    m_displayWidget->beginUpdate();
    m_displayWidget->setCurrentFrameRect(m_currentFrameRect);
    m_displayWidget->setCncPosition(cncXMm, cncYMm);

    int startCol = std::max(0, frameRect.left() / TILE_SIZE);
    int endCol = std::min(m_cols - 1, frameRect.right() / TILE_SIZE);
    int startRow = std::max(0, frameRect.top() / TILE_SIZE);
    int endRow = std::min(m_rows - 1, frameRect.bottom() / TILE_SIZE);

    QImage scaledFrame = cameraFrame.scaled(scaledW, scaledH,
                                            Qt::IgnoreAspectRatio, Qt::FastTransformation)
                             .convertToFormat(QImage::Format_RGB32);

    for (int r = startRow; r <= endRow; ++r) {
        for (int c = startCol; c <= endCol; ++c) {
            auto key = qMakePair(r, c);

            // Lazy tile allocation
            if (!m_tiles.contains(key)) {
                int tw = std::min(TILE_SIZE, m_mosaicWidthPx - c * TILE_SIZE);
                int th = std::min(TILE_SIZE, m_mosaicHeightPx - r * TILE_SIZE);
                QImage img(tw, th, QImage::Format_RGB32);
                img.fill(Qt::white);
                m_tiles[key] = img;
                QImage cov(tw, th, QImage::Format_Grayscale8);
                cov.fill(0);
                m_tileCoverage[key] = cov;
            }

            QImage &tile = m_tiles[key];
            QImage &coverage = m_tileCoverage[key];
            int tileX = c * TILE_SIZE;
            int tileY = r * TILE_SIZE;
            QRect tileRect(tileX, tileY, tile.width(), tile.height());
            QRect intersection = frameRect.intersected(tileRect);
            if (intersection.isEmpty()) continue;

            int destX = intersection.x() - tileX;
            int destY = intersection.y() - tileY;
            int srcX = intersection.x() - frameRect.x();
            int srcY = intersection.y() - frameRect.y();
            int srcW = intersection.width();
            int srcH = intersection.height();

            QImage chunk = scaledFrame.copy(srcX, srcY, srcW, srcH)
                               .mirrored(true, true);
            blendIntoTile(tile, coverage, destX, destY, chunk);
            m_displayWidget->updateTile(r, c, tile);
        }
    }

    m_displayWidget->endUpdate();

    m_positionLabel->setText(QString("CNC: %1 mm, %2 mm")
                                 .arg(cncXMm, 0, 'f', 1)
                                 .arg(cncYMm, 0, 'f', 1));
}

QPixmap MosaicPanel::createPreview(const QSize &size) const
{
    if (!m_displayWidget)
        return {};
    return m_displayWidget->createViewportPreview(size);
}

void MosaicPanel::blendIntoTile(QImage &tile, QImage &coverage,
                                int destX, int destY, const QImage &source)
{
    QImage src32 = source.convertToFormat(QImage::Format_RGB32);
    int w = src32.width();
    int h = src32.height();
    if (w <= 0 || h <= 0) return;

    for (int y = 0; y < h; ++y) {
        if (destY + y >= tile.height()) break;
        const QRgb *srcLine = reinterpret_cast<const QRgb *>(src32.constScanLine(y));
        QRgb *tileLine = reinterpret_cast<QRgb *>(tile.scanLine(destY + y));
        uchar *covLine = coverage.scanLine(destY + y);

        for (int x = 0; x < w; ++x) {
            int dx = destX + x;
            if (dx >= tile.width()) break;
            if (covLine[dx]) {
                // Fast 50/50 average via bit manipulation
                tileLine[dx] = ((tileLine[dx] >> 1) & 0x7F7F7F7F)
                             + ((srcLine[x] >> 1) & 0x7F7F7F7F);
            } else {
                tileLine[dx] = srcLine[x];
            }
            covLine[dx] = 255;
        }
    }
}

void MosaicPanel::onMosaicClicked(double imgX, double imgY)
{
    double mmX = imgX / (m_calibrationPxPerMm * SCALE_FACTOR);
    double mmY = m_stageHeightMm - imgY / (m_calibrationPxPerMm * SCALE_FACTOR);
    emit requestMove(mmX, mmY);
}

void MosaicPanel::onSelectionsChanged(const QVector<QRectF> &qrectfList)
{
    QVector<QRectF> mmRects;
    for (const auto &r : qrectfList) {
        double x1 = r.x() / (m_calibrationPxPerMm * SCALE_FACTOR);
        double y1 = m_stageHeightMm - r.y() / (m_calibrationPxPerMm * SCALE_FACTOR);
        double x2 = r.right() / (m_calibrationPxPerMm * SCALE_FACTOR);
        double y2 = m_stageHeightMm - r.bottom() / (m_calibrationPxPerMm * SCALE_FACTOR);
        mmRects.append(QRectF(QPointF(std::min(x1, x2), std::min(y1, y2)),
                              QPointF(std::max(x1, x2), std::max(y1, y2))));
    }
    emit selectionsChanged(mmRects);

    // Emit as scan request for the last selection
    if (!mmRects.isEmpty()) {
        const auto &last = mmRects.last();
        emit requestScan(last.left(), last.top(), last.right(), last.bottom());
    }
}

void MosaicPanel::onMosaicMouseMoved(double imgX, double imgY)
{
    if (m_calibrationPxPerMm > 0 && m_cursorLabel) {
        double mmX = imgX / (m_calibrationPxPerMm * SCALE_FACTOR);
        double mmY = m_stageHeightMm - imgY / (m_calibrationPxPerMm * SCALE_FACTOR);
        m_cursorLabel->setText(QString("Cursor: %1 mm, %2 mm")
                                   .arg(mmX, 0, 'f', 2)
                                   .arg(mmY, 0, 'f', 2));
    }
}
