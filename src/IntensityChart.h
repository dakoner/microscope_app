#ifndef INTENSITYCHART_H
#define INTENSITYCHART_H

#include <QWidget>
#include <QVector>

class IntensityChart : public QWidget
{
    Q_OBJECT
public:
    explicit IntensityChart(QWidget *parent = nullptr);

    void setData(const QVector<int> &data);

    QSize sizeHint() const override { return QSize(400, 300); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVector<int> m_data;
};

#endif // INTENSITYCHART_H
