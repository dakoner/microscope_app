#ifndef COLORPICKERWIDGET_H
#define COLORPICKERWIDGET_H

#include <QWidget>
#include <QLabel>

class ColorPickerWidget : public QWidget
{
    Q_OBJECT
public:
    explicit ColorPickerWidget(QWidget *parent = nullptr);

    void updateColor(int x, int y, int r, int g, int b);

private:
    QLabel *m_lblCoords = nullptr;
    QLabel *m_lblRgb = nullptr;
    QLabel *m_lblHex = nullptr;
    QLabel *m_lblIntensity = nullptr;
    QLabel *m_lblPreview = nullptr;

    void setupUi();
};

#endif // COLORPICKERWIDGET_H
