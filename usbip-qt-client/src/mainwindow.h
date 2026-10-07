#pragma once

#include <QMainWindow>
#include <QFutureWatcher>
#include <QVector>

#include "remote_client.h"
#include "vhci_client.h"

class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void onRefresh();
    void onRefreshFinished();
    void onAddFavorite();
    void onAttachRow(int row);
    void onDetachRow(int row);
    void refreshImported();

private:
    void buildUi();
    void applyStyle();
    void log(const QString &msg);

    void loadFavorites();
    void saveFavorite(const QString &host);

    QString currentHost() const;
    quint16 currentPort() const;

    QComboBox *m_hostCombo = nullptr;
    QSpinBox *m_portSpin = nullptr;
    QPushButton *m_addFavBtn = nullptr;
    QPushButton *m_refreshBtn = nullptr;
    QTableWidget *m_remoteTable = nullptr;
    QTableWidget *m_importedTable = nullptr;
    QPlainTextEdit *m_logView = nullptr;

    QFutureWatcher<QVector<RemoteDevice>> m_watcher;
    VhciClient m_vhci;

    QVector<RemoteDevice> m_remoteDevices;
    QString m_lastError;
};