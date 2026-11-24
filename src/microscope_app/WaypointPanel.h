#ifndef WAYPOINTPANEL_H
#define WAYPOINTPANEL_H

#include <QWidget>
#include <QCheckBox>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QString>
#include <QVector>

struct Waypoint {
    QString id;
    QString name;
    double xMm = 0.0;
    double yMm = 0.0;
    double zMm = 0.0;
    QString createdAtIso;
    QString thumbnailFile;
    QPixmap thumbnail;
};

class WaypointPanel : public QWidget
{
    Q_OBJECT
public:
    explicit WaypointPanel(QWidget *parent = nullptr);

    void setStoragePaths(const QString &jsonFilePath, const QString &thumbnailsDirPath);
    bool loadWaypoints();
    bool saveWaypoints() const;

    void setCurrentPosition(double xMm, double yMm, double zMm, bool valid = true);

    const QVector<Waypoint> &waypoints() const { return m_waypoints; }
    const Waypoint *findWaypointByIdOrName(const QString &idOrName) const;

    Waypoint addWaypoint(const QString &name, double xMm, double yMm, double zMm,
                         const QImage &snapshot = QImage());
    bool updateWaypoint(const QString &id, double xMm, double yMm, double zMm,
                        const QImage &snapshot = QImage());
    bool renameWaypoint(const QString &id, const QString &newName);
    bool removeWaypointByIdOrName(const QString &idOrName, QString *removedNameOut = nullptr);
    void clearWaypoints();

    bool includeZOnReturn() const;
    void setIncludeZOnReturn(bool includeZ);

signals:
    void saveCurrentRequested(const QString &name);
    void updateCurrentRequested(const QString &waypointId);
    void gotoWaypointRequested(const QString &id, const QString &name,
                               double xMm, double yMm, double zMm, bool includeZ);
    void waypointsChanged();
    void logSignal(const QString &msg);

private slots:
    void onSaveClicked();
    void onGoToSelectedClicked();
    void onRenameSelectedClicked();
    void onUpdateSelectedClicked();
    void onDeleteSelectedClicked();
    void onClearAllClicked();
    void onItemDoubleClicked(QListWidgetItem *item);
    void updateActionButtons();

private:
    void setupUi();
    void rebuildList(const QString &selectId = QString());
    QWidget *createRowWidget(const Waypoint &wp);
    QString selectedWaypointId() const;
    QString nextDefaultName() const;
    QString saveThumbnailImage(const QString &waypointId, const QImage &snapshot) const;
    void deleteThumbnailFile(const QString &thumbnailFile) const;
    QPixmap loadThumbnailPixmap(const QString &thumbnailFile) const;
    void emitGoToForId(const QString &id);

    QString m_jsonFilePath;
    QString m_thumbnailsDirPath;
    QVector<Waypoint> m_waypoints;

    QLabel *m_currentPosLabel = nullptr;
    QLineEdit *m_nameEdit = nullptr;
    QPushButton *m_saveButton = nullptr;
    QCheckBox *m_includeZCheck = nullptr;
    QListWidget *m_listWidget = nullptr;
    QPushButton *m_gotoButton = nullptr;
    QPushButton *m_renameButton = nullptr;
    QPushButton *m_updateButton = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QPushButton *m_clearButton = nullptr;
};

#endif // WAYPOINTPANEL_H
