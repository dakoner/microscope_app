#ifndef HISTOGRAMWIDGET_H
#define HISTOGRAMWIDGET_H

#include <QWidget>
#include <array>

class QImage;

class HistogramWidget : public QWidget
{
    Q_OBJECT

public:
    explicit HistogramWidget(QWidget *parent = nullptr);

    // Rebuild the 256-bin luminance histogram for the latest camera frame.
    void setImage(const QImage &image);
    void setPointerIntensity(int x, int y, int intensity);
    void clearPointerIntensity();

    QSize sizeHint() const override { return QSize(400, 300); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    std::array<quint64, 256> m_bins{};
    quint64 m_maxBinCount = 0;
    quint64 m_pixelCount = 0;
    double m_meanIntensity = 0.0;
    int m_pointerX = -1;
    int m_pointerY = -1;
    int m_pointerIntensity = -1;
};

#endif // HISTOGRAMWIDGET_H
