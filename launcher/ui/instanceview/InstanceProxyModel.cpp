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

#include "InstanceProxyModel.h"

#include <BaseInstance.h>
#include <icons/IconList.h>
#include "Application.h"
#include "InstanceSummary.h"
#include "InstanceView.h"

#include <QDebug>

InstanceProxyModel::InstanceProxyModel(QObject* parent) : QSortFilterProxyModel(parent)
{
    m_naturalSort.setNumericMode(true);
    m_naturalSort.setCaseSensitivity(Qt::CaseSensitivity::CaseInsensitive);
    // FIXME: use loaded translation as source of locale instead, hook this up to translation changes
    m_naturalSort.setLocale(QLocale::system());
}

const QString& InstanceProxyModel::favoritesGroup()
{
    static const QString id = QChar(1) + QStringLiteral("favorites");
    return id;
}

void InstanceProxyModel::setFilterQuery(const QString& query)
{
    const auto words = query.toLower().split(' ', Qt::SkipEmptyParts);
    if (words == m_queryWords)
        return;
    m_queryWords = words;
    invalidateFilter();
}

QString InstanceProxyModel::groupOf(const QModelIndex& sourceIndex) const
{
    auto* instance = static_cast<BaseInstance*>(sourceIndex.internalPointer());
    if (instance && instance->isFavorite())
        return favoritesGroup();
    return sourceIndex.data(InstanceViewRoles::GroupRole).toString();
}

QVariant InstanceProxyModel::data(const QModelIndex& index, int role) const
{
    if (role == InstanceViewRoles::GroupRole)
        return groupOf(mapToSource(index));
    QVariant data = QSortFilterProxyModel::data(index, role);
    if (role == Qt::DecorationRole) {
        return QVariant(APPLICATION->icons()->getIcon(data.toString()));
    }
    return data;
}

bool InstanceProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    if (m_queryWords.isEmpty())
        return true;
    const auto index = sourceModel()->index(sourceRow, 0, sourceParent);
    auto* instance = static_cast<BaseInstance*>(index.internalPointer());
    if (!instance)
        return false;
    const auto text = InstanceSummary::searchText(instance, index.data(InstanceViewRoles::GroupRole).toString());
    for (const auto& word : m_queryWords) {
        if (!text.contains(word))
            return false;
    }
    return true;
}

bool InstanceProxyModel::lessThan(const QModelIndex& left, const QModelIndex& right) const
{
    const QString leftCategory = groupOf(left);
    const QString rightCategory = groupOf(right);
    if (leftCategory == rightCategory) {
        return subSortLessThan(left, right);
    } else if (leftCategory == favoritesGroup() || rightCategory == favoritesGroup()) {
        return leftCategory == favoritesGroup();
    } else {
        // FIXME: real group sorting happens in InstanceView::updateGeometries(), see LocaleString
        auto result = leftCategory.localeAwareCompare(rightCategory);
        if (result == 0) {
            return subSortLessThan(left, right);
        }
        return result < 0;
    }
}

bool InstanceProxyModel::subSortLessThan(const QModelIndex& left, const QModelIndex& right) const
{
    BaseInstance* pdataLeft = static_cast<BaseInstance*>(left.internalPointer());
    BaseInstance* pdataRight = static_cast<BaseInstance*>(right.internalPointer());
    QString sortMode = APPLICATION->settings()->get("InstSortMode").toString();
    if (sortMode == "LastLaunch") {
        return pdataLeft->lastLaunch() > pdataRight->lastLaunch();
    } else {
        return m_naturalSort.compare(pdataLeft->name(), pdataRight->name()) < 0;
    }
}
