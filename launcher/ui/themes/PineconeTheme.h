// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "FusionTheme.h"

/// The PineconeMC look: floating cards on charcoal with the Prism green as accent.
/// The style sheet and its images live in :/themes/pinecone.
class PineconeTheme : public FusionTheme {
   public:
    virtual ~PineconeTheme() {}

    QString id() override;
    QString name() override;
    QString tooltip() override;
    bool hasStyleSheet() override;
    QString appStyleSheet() override;
    QPalette colorScheme() override;
    double fadeAmount() override;
    QColor fadeColor() override;
    LogColors logColorScheme() override;
    QStringList searchPaths() override;
};
