// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "Widgets/FilterBarTableViewWidget.h"

#include <qlineedit.h>
#include <qsortfilterproxymodel.h>

#include "ui_FilterBarTableViewWidget.h"

namespace Widgets {

FilterBarTableViewWidget::FilterBarTableViewWidget(QWidget* parent)
    : QWidget(parent), ui(new Ui::FilterBarTableViewWidget) {
  ui->setupUi(this);
  ui->tableView->setModel(&proxy_model_);

  // QObject::connect(ui->FilterLineEdit, &QLineEdit::textChanged,
  // &proxy_model_,
  //                  &QSortFilterProxyModel::setFilterWildcard);
  // QObject::connect(ui->FilterLineEdit, &QLineEdit::textChanged,
  // &proxy_model_,
  //                  &QSortFilterProxyModel::setFilterFixedString);

  QObject::connect(ui->FilterLineEdit, &QLineEdit::textChanged, &proxy_model_,
                   qOverload<const QString&>(
                       &QSortFilterProxyModel::setFilterRegularExpression));

  // QObject::connect(ui->FilterLineEdit, &QLineEdit::textChanged,
  // &proxy_model_,
  //                  [this](const QString& pattern) {
  //                    const QString& trimmed = pattern.trimmed();
  //                    if (trimmed.size() >= 3) {
  //                      proxy_model_.setFilterRegularExpression(trimmed);
  //                    } else {
  //                      proxy_model_.setFilterRegularExpression("");
  //                    }
  //                  });
  // QObject::connect(ui->FilterLineEdit, &QLineEdit::textChanged,
  // &proxy_model_,
  //                  &CustomSortFilterProxyModel::setFilterText);
}

QTableView* FilterBarTableViewWidget::GetTableView() { return ui->tableView; }
const QTableView* FilterBarTableViewWidget::GetTableView() const {
  return ui->tableView;
}

QLabel* FilterBarTableViewWidget::GetLabel() { return ui->label; }
const QLabel* FilterBarTableViewWidget::GetLabel() const { return ui->label; }

FilterBarTableViewWidget::~FilterBarTableViewWidget() = default;

}  // namespace Widgets
