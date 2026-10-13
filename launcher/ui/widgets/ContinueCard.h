// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QFrame>
#include <QPointer>

class BaseInstance;
class QLabel;
class QPushButton;
class QTimer;
class QToolButton;

/// Banner above the instance grid with the most recently played instance:
/// one click to play it again, or to jump straight back into its last world.
class ContinueCard : public QFrame {
    Q_OBJECT

   public:
    explicit ContinueCard(QWidget* parent = nullptr);

    /// The card only shows when allowed and there is something to continue.
    void setAllowed(bool allowed);

   signals:
    /// The close button was clicked.
    void hideRequested();

   public slots:
    /// Re-pick the most recently played instance.
    void refresh();

   protected:
    void changeEvent(QEvent* event) override;

   private:
    void scheduleRefresh();
    void setInstance(BaseInstance* instance);
    void updateContents();
    void updateMutedColors();
    void launch(bool intoWorld);

    QPointer<BaseInstance> m_instance;
    qint64 m_scannedLaunch = -1;  // last launch time the world was looked up for
    QString m_worldFolder;
    bool m_allowed = true;
    bool m_refreshQueued = false;

    QLabel* m_icon = nullptr;
    QLabel* m_caption = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_details = nullptr;
    QPushButton* m_worldButton = nullptr;
    QPushButton* m_playButton = nullptr;
    QToolButton* m_closeButton = nullptr;
    QTimer* m_clock = nullptr;
};
