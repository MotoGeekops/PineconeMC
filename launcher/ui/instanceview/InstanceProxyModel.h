/* Copyright 2013-2021 MultiMC Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <QCollator>
#include <QSortFilterProxyModel>
#include <QStringList>

class InstanceProxyModel : public QSortFilterProxyModel {
    Q_OBJECT

   public:
    InstanceProxyModel(QObject* parent = 0);

    /// Group id given to favourite instances, so they form their own group above all others.
    /// It starts with a control character, so it can't clash with a group the user typed.
    static const QString& favoritesGroup();

    /// Keep only instances whose name, group, Minecraft version or mod loader contain every word of the query.
    void setFilterQuery(const QString& query);
    bool isFiltering() const { return !m_queryWords.isEmpty(); }

   protected:
    QVariant data(const QModelIndex& index, int role) const override;
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;
    bool subSortLessThan(const QModelIndex& left, const QModelIndex& right) const;

   private:
    /// The group an instance is shown in: its own, or favoritesGroup() when it is a favourite.
    QString groupOf(const QModelIndex& sourceIndex) const;

    QCollator m_naturalSort;
    QStringList m_queryWords;
};
