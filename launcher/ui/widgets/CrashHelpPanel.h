// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QFrame>

#include "minecraft/CrashAnalyzer.h"

class MinecraftInstance;
class QLabel;
class QVBoxLayout;

/// Banner above the game log that explains why the game crashed, with buttons that fix the usual causes.
class CrashHelpPanel : public QFrame {
    Q_OBJECT
   public:
    explicit CrashHelpPanel(QWidget* parent = nullptr);

    /// Shows the diagnoses for @p instance, or hides the panel when there are none.
    void showDiagnoses(MinecraftInstance* instance, const QList<CrashAnalyzer::Diagnosis>& diagnoses);

    /**
     * Carries out @p fix for @p instance. Returns a short confirmation for fixes that change something,
     * or an empty string for the ones that only open a page, file or website.
     */
    static QString applyFix(MinecraftInstance* instance, const CrashAnalyzer::Fix& fix);

   protected:
    void changeEvent(QEvent* event) override;

   private:
    void updateMutedColors();

    MinecraftInstance* m_instance = nullptr;
    QLabel* m_icon = nullptr;
    QLabel* m_caption = nullptr;
    QWidget* m_content = nullptr;
    QList<QLabel*> m_mutedLabels;
};
