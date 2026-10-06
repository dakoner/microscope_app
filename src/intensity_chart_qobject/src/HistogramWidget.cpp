#include "HistogramWidget.h"

#include <QImage>
#include <QPainter>

HistogramWidget::HistogramWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(220);
}

void HistogramWidget::setImage(const QImage &image)
{
    m_bins.fill(0);
    m_maxBinCount = 0;
    m_pixelCount = 0;
    m_meanIntensity = 0.0;

    if (image.isNull()) {
        update();
        return;
    }

    const QImage rgbImage = image.convertToFormat(QImage::Format_RGB32);
    quint64 intensitySum = 0;
    for (int y = 0; y < rgbImage.height(); ++y) {
        const auto *scanLine = reinterpret_cast<const QRgb *>(rgbImage.constScanLine(y));
        for (int x = 0; x < rgbImage.width(); ++x) {
            const QRgb pixel = scanLine[x];
            // Rec. 709 luma, rounded to the nearest 8-bit intensity.
            const int luminance = (54 * qRed(pixel) + 183 * qGreen(pixel) + 19 * qBlue(pixel) + 128) >> 8;
            ++m_bins[static_cast<size_t>(luminance)];
            intensitySum += static_cast<quint64>(luminance);
        }
    }

    m_pixelCount = static_cast<quint64>(rgbImage.width()) * rgbImage.height();
    m_meanIntensity = m_pixelCount ? static_cast<double>(intensitySum) / m_pixelCount : 0.0;
    for (const quint64 count : m_bins)
        m_maxBinCount = qMax(m_maxBinCount, count);
    update();
}

void HistogramWidget::setPointerIntensity(int x, int y, int intensity)
{
    m_pointerX = x;
    m_pointerY = y;
    m_pointerIntensity = intensity;
    update();
}

void HistogramWidget::clearPointerIntensity()
{
    if (m_pointerIntensity < 0)
        return;
    m_pointerX = -1;
    m_pointerY = -1;
    m_pointerIntensity = -1;
    update();
}

void HistogramWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor(30, 30, 30));

    const QRect plot = rect().adjusted(36, 18, -14, -36);
    if (m_pixelCount == 0 || plot.width() <= 0 || plot.height() <= 0) {
        painter.setPen(QColor(150, 150, 150));
        painter.drawText(rect(), Qt::AlignCenter, "Waiting for image data");
        return;
    }

    painter.setPen(QPen(QColor(200, 200, 200), 1));
    painter.drawLine(plot.bottomLeft(), plot.bottomRight());
    painter.drawLine(plot.bottomLeft(), plot.topLeft());

    painter.setPen(QPen(QColor(70, 70, 70), 1, Qt::DotLine));
    for (int intensity : {64, 128, 192}) {
        const int x = plot.left() + intensity * plot.width() / 255;
        painter.drawLine(x, plot.top(), x, plot.bottom());
    }

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(70, 190, 255));
    for (int intensity = 0; intensity < 256; ++intensity) {
        const int x1 = plot.left() + intensity * plot.width() / 256;
        const int x2 = plot.left() + (intensity + 1) * plot.width() / 256;
        const int barHeight = static_cast<int>(m_bins[static_cast<size_t>(intensity)]
            * static_cast<quint64>(plot.height()) / m_maxBinCount);
        painter.drawRect(x1, plot.bottom() - barHeight + 1, qMax(1, x2 - x1), barHeight);
    }

    if (m_pointerIntensity >= 0) {
        const int x = plot.left() + m_pointerIntensity * plot.width() / 255;
        painter.setPen(QPen(QColor(255, 190, 0), 2));
        painter.drawLine(x, plot.top(), x, plot.bottom());
    }

    painter.setPen(QColor(220, 220, 220));
    painter.drawText(plot.left() - 4, plot.bottom() + 17, "0");
    painter.drawText(plot.left() + plot.width() / 2 - 10, plot.bottom() + 17, "128");
    painter.drawText(plot.right() - 18, plot.bottom() + 17, "255");
    QString summary = QString("Pixels: %1   Mean: %2")
        .arg(m_pixelCount).arg(m_meanIntensity, 0, 'f', 1);
    if (m_pointerIntensity >= 0)
        summary += QString("   Cursor (%1, %2): %3")
            .arg(m_pointerX).arg(m_pointerY).arg(m_pointerIntensity);
    painter.drawText(plot.left(), 13, summary);
}
