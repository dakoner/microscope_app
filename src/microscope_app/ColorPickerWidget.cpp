#include "ColorPickerWidget.h"
#include <QVBoxLayout>
#include <QFormLayout>

ColorPickerWidget::ColorPickerWidget(QWidget *parent)
    : QWidget(parent)
{
    setupUi();
}

void ColorPickerWidget::setupUi()
{
    auto *layout = new QFormLayout(this);
    m_lblCoords = new QLabel("0, 0");
    m_lblRgb = new QLabel("0, 0, 0");
    m_lblHex = new QLabel("#000000");
    m_lblIntensity = new QLabel("0");
    m_lblPreview = new QLabel;
    m_lblPreview->setFixedSize(60, 30);
    m_lblPreview->setStyleSheet("background-color: #000000; border: 1px solid gray;");

    layout->addRow("Coords:", m_lblCoords);
    layout->addRow("RGB:", m_lblRgb);
    layout->addRow("Hex:", m_lblHex);
    layout->addRow("Intensity:", m_lblIntensity);
    layout->addRow("Color:", m_lblPreview);
}

void ColorPickerWidget::updateColor(int x, int y, int r, int g, int b)
{
    m_lblCoords->setText(QString("%1, %2").arg(x).arg(y));
    m_lblRgb->setText(QString("%1, %2, %3").arg(r).arg(g).arg(b));
    QString hex = QString("#%1%2%3")
                      .arg(r, 2, 16, QChar('0'))
                      .arg(g, 2, 16, QChar('0'))
                      .arg(b, 2, 16, QChar('0'))
                      .toUpper();
    m_lblHex->setText(hex);
    m_lblIntensity->setText(QString::number((r + g + b) / 3));
    m_lblPreview->setStyleSheet(
        QString("background-color: %1; border: 1px solid gray;").arg(hex.toLower()));
}
