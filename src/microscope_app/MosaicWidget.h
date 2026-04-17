#ifndef MOSAICWIDGET_H
#define MOSAICWIDGET_H

#include <QWidget>
#include <QImage>
#include <QMap>
#include <QPointF>
#include <QRectF>
#include <QScrollBar>
#include <QPair>

class MosaicWidget : public QWidget
{
    Q_OBJECT
public:
    explicit MosaicWidget(QWidget *parent = nullptr);

    void setupScrollbars(QScrollBar *hBar, QScrollBar *vBar);
    void setStageSize(double widthMm, double heightMm);
    void setCalibration(double pxPerMm);
    void setGridSize(int width, int height);
    void setCurrentFrameRect(const QRectF &rect);
    void setCncPosition(double xMm, double yMm);
    void setOverlayCircles(const QVector<QPair<QPointF, double>> &circles);
    void resetMosaic(int width, int height, int tileSize);
    void updateTile(int row, int col, const QImage &image);
    void beginUpdate();
    void endUpdate();
    QPointF widgetToImageCoords(const QPointF &widgetPos) const;
    QPointF imageToWidgetCoords(const QPointF &imagePos) const;
    void fitToWindow();
    QPixmap createViewportPreview(const QSize &size) const;

signals:
    void clicked(double imgX, double imgY);
    void selectionsChanged(const QVector<QRectF> &rects);
    void mouseMoved(double imgX, double imgY);
    void selectionMade(double x, double y, double w, double h);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void doZoom(int direction, QPointF center = QPointF());
    void clampPanOffset();
    void updateScrollbars();
    void drawAxes(QPainter &painter);

    QMap<QPair<int,int>, QImage> m_tiles;
    int m_tileSize = 32768;
    int m_totalWidth = 0;
    int m_totalHeight = 0;
    int m_gridWidth = 0;
    int m_gridHeight = 0;

    double m_zoomFactor = 1.0;
    QPointF m_panOffset;
    QPointF m_lastMousePos;

    bool m_isPanning = false;
    bool m_isSelecting = false;

    double m_stageWidthMm = 0;
    double m_stageHeightMm = 0;
    double m_calibrationPxPerMm = 0;

    QRectF m_currentFrameRect;
    QPointF m_cncImagePos;  // CNC position in mosaic image coords
    QVector<QRectF> m_selectedRects;
    QVector<QPair<QPointF, double>> m_overlayCircles;

    QPointF m_selStartPos;
    QPointF m_selCurrentPos;

    QScrollBar *m_hScrollbar = nullptr;
    QScrollBar *m_vScrollbar = nullptr;
    bool m_updatingScrollbars = false;
    bool m_batchUpdate = false;
    bool m_hasInitializedView = false;
};

#endif // MOSAICWIDGET_H
