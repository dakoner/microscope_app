#ifndef MOSAICPANEL_H
#define MOSAICPANEL_H

#include <QWidget>
#include <QImage>
#include <QLabel>
#include <QScrollBar>
#include <QMap>
#include <QPair>
#include <QRectF>

#include "MosaicWidget.h"

class MosaicPanel : public QWidget
{
    Q_OBJECT
public:
    static constexpr int TILE_SIZE = 512;
    static constexpr double SCALE_FACTOR = 0.1;

    MosaicPanel(double stageWidthMm, double stageHeightMm,
                double rulerCalibrationPxPerMm, QWidget *parent = nullptr);

    void updateMosaic(const QImage &cameraFrame, double cncXMm, double cncYMm);
    void setCncPosition(double xMm, double yMm);
    void setStageCircles(const QVector<std::tuple<double, double, double>> &circlesMm);
    QPixmap createPreview(const QSize &size) const;

signals:
    void requestMove(double x, double y);
    void requestScan(double xMin, double yMin, double xMax, double yMax);
    void selectionsChanged(const QVector<QRectF> &mmRects);

private slots:
    void onMosaicClicked(double imgX, double imgY);
    void onSelectionsChanged(const QVector<QRectF> &qrectfList);
    void onMosaicMouseMoved(double imgX, double imgY);

private:
    void blendIntoTile(QImage &tile, QImage &coverage, int destX, int destY,
                       const QImage &source);

    double m_stageWidthMm;
    double m_stageHeightMm;
    double m_calibrationPxPerMm;
    int m_mosaicWidthPx;
    int m_mosaicHeightPx;
    int m_cols;
    int m_rows;

    MosaicWidget *m_displayWidget = nullptr;
    QLabel *m_positionLabel = nullptr;
    QLabel *m_cursorLabel = nullptr;

    QMap<QPair<int,int>, QImage> m_tiles;
    QMap<QPair<int,int>, QImage> m_tileCoverage;
    QRectF m_currentFrameRect;

    int m_cameraFrameWidthPx = 0;
    int m_cameraFrameHeightPx = 0;
};

#endif // MOSAICPANEL_H
