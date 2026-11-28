#include "WaypointPanel.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QMouseEvent>
#include <QSaveFile>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <cmath>
#include <limits>

namespace {
constexpr int kStoredThumbMaxW = 240;
constexpr int kStoredThumbMaxH = 180;
constexpr int kDisplayThumbW = 84;
constexpr int kDisplayThumbH = 63;
} // namespace

WaypointPanel::WaypointPanel(QWidget *parent)
    : QWidget(parent)
{
    setupUi();
    updateActionButtons();
}

void WaypointPanel::setupUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(6, 6, 6, 6);
    mainLayout->setSpacing(6);

    // Live stage position header
    m_currentPosLabel = new QLabel(QStringLiteral("Stage: --"));
    m_currentPosLabel->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 11px;"));
    mainLayout->addWidget(m_currentPosLabel);

    // Save current position row
    auto *saveRow = new QHBoxLayout;
    m_nameEdit = new QLineEdit;
    m_nameEdit->setPlaceholderText(QStringLiteral("Waypoint name (optional)"));
    m_saveButton = new QPushButton(QStringLiteral("Save"));
    m_saveButton->setToolTip(QStringLiteral(
        "Save the current (X, Y, Z) stage position with a thumbnail preview from the live camera."));
    saveRow->addWidget(m_nameEdit, 1);
    saveRow->addWidget(m_saveButton);
    mainLayout->addLayout(saveRow);

    // Return option
    m_includeZCheck = new QCheckBox(QStringLiteral("Restore Z focus height on return"));
    m_includeZCheck->setChecked(true);
    m_includeZCheck->setToolTip(QStringLiteral(
        "When checked, returning to a waypoint moves XY first and then restores its saved Z height."));
    mainLayout->addWidget(m_includeZCheck);

    // Waypoint list with thumbnail previews and one-click Go buttons
    m_listWidget = new QListWidget;
    m_listWidget->setSelectionMode(QAbstractItemView::SingleSelection);
    m_listWidget->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_listWidget->setMinimumHeight(200);
    m_listWidget->setAlternatingRowColors(true);
    m_listWidget->viewport()->installEventFilter(this);
    mainLayout->addWidget(m_listWidget, 1);

    // Bottom management bar
    auto *btnRow = new QHBoxLayout;
    btnRow->setSpacing(4);
    m_gotoButton = new QPushButton(QStringLiteral("Go To"));
    m_gotoButton->setToolTip(QStringLiteral("Move stage to the selected waypoint."));
    m_renameButton = new QPushButton(QStringLiteral("Rename"));
    m_updateButton = new QPushButton(QStringLiteral("Update"));
    m_updateButton->setToolTip(QStringLiteral(
        "Overwrite the selected waypoint with the current (X, Y, Z) position and camera thumbnail."));
    m_deleteButton = new QPushButton(QStringLiteral("Delete"));
    m_clearButton = new QPushButton(QStringLiteral("Clear"));
    btnRow->addWidget(m_gotoButton);
    btnRow->addWidget(m_renameButton);
    btnRow->addWidget(m_updateButton);
    btnRow->addWidget(m_deleteButton);
    btnRow->addWidget(m_clearButton);
    mainLayout->addLayout(btnRow);

    connect(m_saveButton, &QPushButton::clicked, this, &WaypointPanel::onSaveClicked);
    connect(m_nameEdit, &QLineEdit::returnPressed, this, &WaypointPanel::onSaveClicked);
    connect(m_gotoButton, &QPushButton::clicked, this, &WaypointPanel::onGoToSelectedClicked);
    connect(m_renameButton, &QPushButton::clicked, this, &WaypointPanel::onRenameSelectedClicked);
    connect(m_updateButton, &QPushButton::clicked, this, &WaypointPanel::onUpdateSelectedClicked);
    connect(m_deleteButton, &QPushButton::clicked, this, &WaypointPanel::onDeleteSelectedClicked);
    connect(m_clearButton, &QPushButton::clicked, this, &WaypointPanel::onClearAllClicked);
    connect(m_listWidget, &QListWidget::itemSelectionChanged, this, &WaypointPanel::updateActionButtons);
    connect(m_listWidget, &QListWidget::itemDoubleClicked, this, &WaypointPanel::onItemDoubleClicked);
}

void WaypointPanel::setStoragePaths(const QString &jsonFilePath, const QString &thumbnailsDirPath)
{
    m_jsonFilePath = jsonFilePath;
    m_thumbnailsDirPath = thumbnailsDirPath;
}

void WaypointPanel::setCurrentPosition(double xMm, double yMm, double zMm, bool valid)
{
    if (!m_currentPosLabel) {
        return;
    }
    if (!valid || !std::isfinite(xMm) || !std::isfinite(yMm) || !std::isfinite(zMm)) {
        m_currentPosLabel->setText(QStringLiteral("Stage: --"));
        return;
    }
    m_currentPosLabel->setText(QString("Stage: X %1  Y %2  Z %3 mm")
                                   .arg(xMm, 0, 'f', 3)
                                   .arg(yMm, 0, 'f', 3)
                                   .arg(zMm, 0, 'f', 3));
}

bool WaypointPanel::includeZOnReturn() const
{
    return !m_includeZCheck || m_includeZCheck->isChecked();
}

void WaypointPanel::setIncludeZOnReturn(bool includeZ)
{
    if (m_includeZCheck) {
        m_includeZCheck->setChecked(includeZ);
    }
}

QString WaypointPanel::nextDefaultName() const
{
    int index = m_waypoints.size() + 1;
    while (true) {
        const QString candidate = QString("Waypoint %1").arg(index);
        bool exists = false;
        for (const Waypoint &wp : m_waypoints) {
            if (wp.name.compare(candidate, Qt::CaseInsensitive) == 0) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            return candidate;
        }
        ++index;
    }
}

QString WaypointPanel::saveThumbnailImage(const QString &waypointId, const QImage &snapshot) const
{
    if (snapshot.isNull() || m_thumbnailsDirPath.isEmpty()) {
        return QString();
    }
    QDir dir(m_thumbnailsDirPath);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        return QString();
    }

    QImage scaled = snapshot;
    if (scaled.width() > kStoredThumbMaxW || scaled.height() > kStoredThumbMaxH) {
        scaled = scaled.scaled(kStoredThumbMaxW, kStoredThumbMaxH,
                               Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    const QString filename = QString("wp_%1.png").arg(waypointId);
    const QString fullPath = dir.filePath(filename);
    if (!scaled.save(fullPath, "PNG")) {
        return QString();
    }
    return filename;
}

void WaypointPanel::deleteThumbnailFile(const QString &thumbnailFile) const
{
    if (thumbnailFile.isEmpty() || m_thumbnailsDirPath.isEmpty()) {
        return;
    }
    const QString baseName = QFileInfo(thumbnailFile).fileName();
    if (baseName.isEmpty() || baseName != thumbnailFile) {
        return;
    }
    const QString fullPath = QDir(m_thumbnailsDirPath).filePath(baseName);
    if (QFile::exists(fullPath)) {
        QFile::remove(fullPath);
    }
}

QPixmap WaypointPanel::loadThumbnailPixmap(const QString &thumbnailFile) const
{
    if (thumbnailFile.isEmpty() || m_thumbnailsDirPath.isEmpty()) {
        return QPixmap();
    }
    const QString baseName = QFileInfo(thumbnailFile).fileName();
    if (baseName.isEmpty()) {
        return QPixmap();
    }
    const QString fullPath = QDir(m_thumbnailsDirPath).filePath(baseName);
    if (!QFile::exists(fullPath)) {
        return QPixmap();
    }
    QPixmap pix(fullPath);
    return pix;
}

bool WaypointPanel::loadWaypoints()
{
    m_waypoints.clear();
    if (m_jsonFilePath.isEmpty() || !QFile::exists(m_jsonFilePath)) {
        rebuildList();
        emit waypointsChanged();
        return false;
    }

    QFile file(m_jsonFilePath);
    if (!file.open(QIODevice::ReadOnly)) {
        emit logSignal(QString("Could not open waypoints file: %1").arg(m_jsonFilePath));
        rebuildList();
        emit waypointsChanged();
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        emit logSignal(QString("Failed to parse waypoints file (%1): %2")
                           .arg(m_jsonFilePath, parseError.errorString()));
        rebuildList();
        emit waypointsChanged();
        return false;
    }

    QJsonArray arr;
    if (doc.isObject()) {
        arr = doc.object().value(QStringLiteral("waypoints")).toArray();
    } else if (doc.isArray()) {
        arr = doc.array();
    }

    for (const QJsonValue &val : arr) {
        if (!val.isObject()) {
            continue;
        }
        const QJsonObject obj = val.toObject();
        const double x = obj.value(QStringLiteral("x_mm")).toDouble(std::numeric_limits<double>::quiet_NaN());
        const double y = obj.value(QStringLiteral("y_mm")).toDouble(std::numeric_limits<double>::quiet_NaN());
        const double z = obj.value(QStringLiteral("z_mm")).toDouble(0.0);
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
            continue;
        }

        Waypoint wp;
        wp.id = obj.value(QStringLiteral("id")).toString().trimmed();
        if (wp.id.isEmpty()) {
            wp.id = QUuid::createUuid().toString(QUuid::Id128).left(12);
        }
        wp.name = obj.value(QStringLiteral("name")).toString().trimmed();
        if (wp.name.isEmpty()) {
            wp.name = nextDefaultName();
        }
        wp.xMm = x;
        wp.yMm = y;
        wp.zMm = z;
        wp.createdAtIso = obj.value(QStringLiteral("created_at")).toString();
        wp.thumbnailFile = obj.value(QStringLiteral("thumbnail_file")).toString();
        wp.thumbnail = loadThumbnailPixmap(wp.thumbnailFile);
        m_waypoints.append(wp);
    }

    rebuildList();
    emit waypointsChanged();
    if (!m_waypoints.isEmpty()) {
        emit logSignal(QString("Loaded %1 saved waypoint(s).").arg(m_waypoints.size()));
    }
    return true;
}

bool WaypointPanel::saveWaypoints() const
{
    if (m_jsonFilePath.isEmpty()) {
        return false;
    }

    const QFileInfo fi(m_jsonFilePath);
    QDir parentDir = fi.dir();
    if (!parentDir.exists() && !parentDir.mkpath(QStringLiteral("."))) {
        return false;
    }

    QJsonArray arr;
    for (const Waypoint &wp : m_waypoints) {
        QJsonObject obj;
        obj[QStringLiteral("id")] = wp.id;
        obj[QStringLiteral("name")] = wp.name;
        obj[QStringLiteral("x_mm")] = wp.xMm;
        obj[QStringLiteral("y_mm")] = wp.yMm;
        obj[QStringLiteral("z_mm")] = wp.zMm;
        obj[QStringLiteral("created_at")] = wp.createdAtIso;
        if (!wp.thumbnailFile.isEmpty()) {
            obj[QStringLiteral("thumbnail_file")] = wp.thumbnailFile;
        }
        arr.append(obj);
    }

    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("waypoints")] = arr;

    QSaveFile file(m_jsonFilePath);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return file.commit();
}

const Waypoint *WaypointPanel::findWaypointByIdOrName(const QString &idOrName) const
{
    const QString key = idOrName.trimmed();
    if (key.isEmpty()) {
        return nullptr;
    }
    for (const Waypoint &wp : m_waypoints) {
        if (wp.id == key) {
            return &wp;
        }
    }
    for (const Waypoint &wp : m_waypoints) {
        if (wp.name.compare(key, Qt::CaseInsensitive) == 0) {
            return &wp;
        }
    }
    return nullptr;
}

Waypoint WaypointPanel::addWaypoint(const QString &name, double xMm, double yMm, double zMm,
                                    const QImage &snapshot)
{
    Waypoint wp;
    wp.id = QUuid::createUuid().toString(QUuid::Id128).left(12);
    wp.name = name.trimmed().isEmpty() ? nextDefaultName() : name.trimmed();
    wp.xMm = xMm;
    wp.yMm = yMm;
    wp.zMm = zMm;
    wp.createdAtIso = QDateTime::currentDateTime().toString(Qt::ISODate);
    wp.thumbnailFile = saveThumbnailImage(wp.id, snapshot);
    if (!snapshot.isNull()) {
        wp.thumbnail = QPixmap::fromImage(
            snapshot.scaled(kStoredThumbMaxW, kStoredThumbMaxH,
                            Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    m_waypoints.append(wp);
    saveWaypoints();
    rebuildList(wp.id);
    emit waypointsChanged();
    return wp;
}

bool WaypointPanel::updateWaypoint(const QString &id, double xMm, double yMm, double zMm,
                                   const QImage &snapshot)
{
    for (Waypoint &wp : m_waypoints) {
        if (wp.id != id) {
            continue;
        }
        wp.xMm = xMm;
        wp.yMm = yMm;
        wp.zMm = zMm;
        wp.createdAtIso = QDateTime::currentDateTime().toString(Qt::ISODate);
        if (!snapshot.isNull()) {
            const QString newFile = saveThumbnailImage(wp.id, snapshot);
            if (!newFile.isEmpty()) {
                wp.thumbnailFile = newFile;
            }
            wp.thumbnail = QPixmap::fromImage(
                snapshot.scaled(kStoredThumbMaxW, kStoredThumbMaxH,
                                Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
        saveWaypoints();
        rebuildList(wp.id);
        emit waypointsChanged();
        return true;
    }
    return false;
}

bool WaypointPanel::renameWaypoint(const QString &id, const QString &newName)
{
    const QString cleaned = newName.trimmed();
    if (cleaned.isEmpty()) {
        return false;
    }
    for (Waypoint &wp : m_waypoints) {
        if (wp.id != id) {
            continue;
        }
        wp.name = cleaned;
        saveWaypoints();
        rebuildList(wp.id);
        emit waypointsChanged();
        return true;
    }
    return false;
}

bool WaypointPanel::removeWaypointByIdOrName(const QString &idOrName, QString *removedNameOut)
{
    const QString key = idOrName.trimmed();
    if (key.isEmpty()) {
        return false;
    }

    int idx = -1;
    for (int i = 0; i < m_waypoints.size(); ++i) {
        if (m_waypoints.at(i).id == key) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        for (int i = 0; i < m_waypoints.size(); ++i) {
            if (m_waypoints.at(i).name.compare(key, Qt::CaseInsensitive) == 0) {
                idx = i;
                break;
            }
        }
    }
    if (idx < 0) {
        return false;
    }

    const Waypoint removed = m_waypoints.takeAt(idx);
    deleteThumbnailFile(removed.thumbnailFile);
    if (removedNameOut) {
        *removedNameOut = removed.name;
    }

    saveWaypoints();
    rebuildList();
    emit waypointsChanged();
    return true;
}

void WaypointPanel::clearWaypoints()
{
    for (const Waypoint &wp : m_waypoints) {
        deleteThumbnailFile(wp.thumbnailFile);
    }
    m_waypoints.clear();
    saveWaypoints();
    rebuildList();
    emit waypointsChanged();
}

void WaypointPanel::rebuildList(const QString &selectId)
{
    const QString targetSelection = selectId.isEmpty() ? selectedWaypointId() : selectId;
    m_listWidget->clear();

    QListWidgetItem *itemToSelect = nullptr;
    for (const Waypoint &wp : m_waypoints) {
        auto *item = new QListWidgetItem(m_listWidget);
        item->setData(Qt::UserRole, wp.id);
        item->setToolTip(QString("%1\nX: %2 mm  Y: %3 mm  Z: %4 mm\nSaved: %5\nDouble-click to go; Ctrl+Click to delete")
                             .arg(wp.name)
                             .arg(wp.xMm, 0, 'f', 3)
                             .arg(wp.yMm, 0, 'f', 3)
                             .arg(wp.zMm, 0, 'f', 3)
                             .arg(wp.createdAtIso));

        QWidget *row = createRowWidget(wp);
        row->setProperty("waypoint_id", wp.id);
        row->installEventFilter(this);
        item->setSizeHint(row->sizeHint().expandedTo(QSize(0, kDisplayThumbH + 10)));
        m_listWidget->addItem(item);
        m_listWidget->setItemWidget(item, row);

        if (!targetSelection.isEmpty() && wp.id == targetSelection) {
            itemToSelect = item;
        }
    }

    if (itemToSelect) {
        m_listWidget->setCurrentItem(itemToSelect);
    }
    updateActionButtons();
}

QWidget *WaypointPanel::createRowWidget(const Waypoint &wp)
{
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(6);

    auto *thumbLabel = new QLabel;
    thumbLabel->setFixedSize(kDisplayThumbW, kDisplayThumbH);
    thumbLabel->setAlignment(Qt::AlignCenter);
    thumbLabel->setStyleSheet(QStringLiteral(
        "background-color: #1e1e1e; border: 1px solid #555; color: #888; font-size: 10px;"));
    if (!wp.thumbnail.isNull()) {
        thumbLabel->setPixmap(wp.thumbnail.scaled(kDisplayThumbW, kDisplayThumbH,
                                                  Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        thumbLabel->setText(QStringLiteral("No preview"));
    }
    layout->addWidget(thumbLabel);

    auto *infoLayout = new QVBoxLayout;
    infoLayout->setContentsMargins(0, 0, 0, 0);
    infoLayout->setSpacing(2);

    auto *nameLabel = new QLabel(wp.name);
    nameLabel->setStyleSheet(QStringLiteral("font-weight: bold;"));
    infoLayout->addWidget(nameLabel);

    auto *coordsLabel = new QLabel(QString("X %1  Y %2  Z %3")
                                       .arg(wp.xMm, 0, 'f', 3)
                                       .arg(wp.yMm, 0, 'f', 3)
                                       .arg(wp.zMm, 0, 'f', 3));
    coordsLabel->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 11px;"));
    infoLayout->addWidget(coordsLabel);

    QString displayTime = wp.createdAtIso;
    const QDateTime dt = QDateTime::fromString(wp.createdAtIso, Qt::ISODate);
    if (dt.isValid()) {
        displayTime = dt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    }
    auto *timeLabel = new QLabel(displayTime);
    timeLabel->setStyleSheet(QStringLiteral("color: #888; font-size: 10px;"));
    infoLayout->addWidget(timeLabel);
    infoLayout->addStretch();

    layout->addLayout(infoLayout, 1);

    auto *goBtn = new QPushButton(QStringLiteral("Go"));
    goBtn->setFixedWidth(44);
    goBtn->setToolTip(QString("One-click return to '%1'").arg(wp.name));
    const QString wpId = wp.id;
    connect(goBtn, &QPushButton::clicked, this, [this, wpId]() {
        // Select the matching list item for visual feedback, then move immediately.
        for (int i = 0; i < m_listWidget->count(); ++i) {
            QListWidgetItem *it = m_listWidget->item(i);
            if (it && it->data(Qt::UserRole).toString() == wpId) {
                m_listWidget->setCurrentItem(it);
                break;
            }
        }
        emitGoToForId(wpId);
    });
    layout->addWidget(goBtn);

    return row;
}

QString WaypointPanel::selectedWaypointId() const
{
    QListWidgetItem *item = m_listWidget ? m_listWidget->currentItem() : nullptr;
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void WaypointPanel::emitGoToForId(const QString &id)
{
    const Waypoint *wp = findWaypointByIdOrName(id);
    if (!wp) {
        return;
    }
    emit gotoWaypointRequested(wp->id, wp->name, wp->xMm, wp->yMm, wp->zMm, includeZOnReturn());
}

void WaypointPanel::updateActionButtons()
{
    const bool hasSelection = !selectedWaypointId().isEmpty();
    const bool hasAny = !m_waypoints.isEmpty();
    if (m_gotoButton) m_gotoButton->setEnabled(hasSelection);
    if (m_renameButton) m_renameButton->setEnabled(hasSelection);
    if (m_updateButton) m_updateButton->setEnabled(hasSelection);
    if (m_deleteButton) m_deleteButton->setEnabled(hasSelection);
    if (m_clearButton) m_clearButton->setEnabled(hasAny);
}

void WaypointPanel::onSaveClicked()
{
    const QString name = m_nameEdit ? m_nameEdit->text().trimmed() : QString();
    if (m_nameEdit) {
        m_nameEdit->clear();
    }
    emit saveCurrentRequested(name);
}

void WaypointPanel::onGoToSelectedClicked()
{
    const QString id = selectedWaypointId();
    if (!id.isEmpty()) {
        emitGoToForId(id);
    }
}

void WaypointPanel::onRenameSelectedClicked()
{
    const QString id = selectedWaypointId();
    const Waypoint *wp = findWaypointByIdOrName(id);
    if (!wp) {
        return;
    }
    bool ok = false;
    const QString newName = QInputDialog::getText(
        this,
        QStringLiteral("Rename Waypoint"),
        QStringLiteral("New name:"),
        QLineEdit::Normal,
        wp->name,
        &ok);
    if (ok && !newName.trimmed().isEmpty()) {
        const QString oldName = wp->name;
        if (renameWaypoint(id, newName)) {
            emit logSignal(QString("Waypoint renamed: '%1' -> '%2'").arg(oldName, newName.trimmed()));
        }
    }
}

void WaypointPanel::onUpdateSelectedClicked()
{
    const QString id = selectedWaypointId();
    if (!id.isEmpty()) {
        emit updateCurrentRequested(id);
    }
}

void WaypointPanel::onDeleteSelectedClicked()
{
    const QString id = selectedWaypointId();
    if (id.isEmpty()) {
        return;
    }
    QString removedName;
    if (removeWaypointByIdOrName(id, &removedName)) {
        emit logSignal(QString("Waypoint deleted: '%1'").arg(removedName));
    }
}

void WaypointPanel::onClearAllClicked()
{
    if (m_waypoints.isEmpty()) {
        return;
    }
    const auto reply = QMessageBox::question(
        this,
        QStringLiteral("Clear All Waypoints"),
        QString("Delete all %1 saved waypoint(s) and their thumbnails?").arg(m_waypoints.size()),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (reply == QMessageBox::Yes) {
        clearWaypoints();
        emit logSignal(QStringLiteral("Cleared all saved waypoints."));
    }
}

void WaypointPanel::onItemDoubleClicked(QListWidgetItem *item)
{
    if (!item) {
        return;
    }
    const QString id = item->data(Qt::UserRole).toString();
    if (!id.isEmpty()) {
        emitGoToForId(id);
    }
}

bool WaypointPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (event && event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton && (me->modifiers() & Qt::ControlModifier)) {
            QString wpId;
            if (m_listWidget && watched == m_listWidget->viewport()) {
                QListWidgetItem *item = m_listWidget->itemAt(me->position().toPoint());
                if (item) {
                    wpId = item->data(Qt::UserRole).toString();
                }
            } else if (watched && watched->property("waypoint_id").isValid()) {
                wpId = watched->property("waypoint_id").toString();
            }
            if (!wpId.isEmpty()) {
                // Defer removal out of the row widget's own event dispatch to avoid
                // destroying the watched widget while it is still on the call stack.
                QTimer::singleShot(0, this, [this, wpId]() {
                    QString removedName;
                    if (removeWaypointByIdOrName(wpId, &removedName)) {
                        emit logSignal(QString("Waypoint deleted (Ctrl+Click): '%1'").arg(removedName));
                    }
                });
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}
