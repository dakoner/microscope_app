#include "IntensityChart.h"
#include <QPainter>
#include <QPen>

IntensityChart::IntensityChart(QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle("Intensity Profile");
    resize(400, 300);
    setAttribute(Qt::WA_DeleteOnClose, false);
}

void IntensityChart::setData(const QVector<int> &data)
{
    m_data = data;
    update();
}

void IntensityChart::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor(30, 30, 30));

    if (m_data.isEmpty()) {
        painter.setPen(QColor(150, 150, 150));
        painter.drawText(rect(), Qt::AlignCenter, "No Data");
        return;
    }

    int w = width();
    int h = height();
    int margin = 20;

    // Axes
    painter.setPen(QPen(QColor(200, 200, 200), 1));
    painter.drawLine(margin, h - margin, w - margin, h - margin);
    painter.drawLine(margin, margin, margin, h - margin);

    int maxVal = 255;
    int count = m_data.size();
    if (count < 2)
        return;

    double stepX = double(w - 2 * margin) / (count - 1);
    double scaleY = double(h - 2 * margin) / maxVal;

    // Grid line at 128
    painter.setPen(QPen(QColor(60, 60, 60), 1, Qt::DotLine));
    int midY = h - margin - int(128 * scaleY);
    painter.drawLine(margin, midY, w - margin, midY);

    // Data line
    painter.setPen(QPen(QColor(0, 255, 0), 2));
    QVector<QPointF> points;
    points.reserve(count);
    for (int i = 0; i < count; ++i) {
        double x = margin + i * stepX;
        double y = h - margin - m_data[i] * scaleY;
        points.append(QPointF(x, y));
    }
    painter.drawPolyline(points.data(), points.size());

    // Labels
    painter.setPen(QColor(255, 255, 255));
    painter.drawText(5, h - margin + 15, "0");
    painter.drawText(5, margin + 10, "255");
}
