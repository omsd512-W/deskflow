/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ComputerSettingsDialog.h"
#include "ui_ComputerSettingsDialog.h"

#include "gui/config/Computer.h"
#include "gui/config/ComputerList.h"
#include "validators/AliasValidator.h"
#include "validators/ComputerLineNameValidator.h"
#include "validators/ValidationError.h"

#include <QBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QMessageBox>
#include <QSpinBox>

#include <common/Settings.h>

using enum ComputerConfig::SwitchCorner;
using enum ComputerConfig::Fix;

ComputerSettingsDialog::~ComputerSettingsDialog() = default;

ComputerSettingsDialog::ComputerSettingsDialog(QWidget *parent, Computer *computer, const ComputerList *computers)
    : QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint),
      ui{std::make_unique<Ui::ComputerSettingsDialog>()},
      m_computer(computer)
{

  ui->setupUi(this);
  ui->buttonBox->button(QDialogButtonBox::Cancel)->setFocus();

  ui->lineNameEdit->setText(m_computer->name());

  const auto valNameError = new validators::ValidationError(this, ui->lblNameError);
  const auto valName = new validators::ComputerLineNameValidator(ui->lineNameEdit, valNameError, computers);
  ui->lineNameEdit->setValidator(valName);

  const auto valAliasError = new validators::ValidationError(this, ui->lblAliasError);
  const auto valAlias = new validators::AliasValidator(ui->lineAddAlias, valAliasError);
  ui->lineAddAlias->setValidator(valAlias);

  for (int i = 0; i < m_computer->aliases().count(); i++)
    new QListWidgetItem(m_computer->aliases()[i], ui->listAliases);

  ui->comboShift->setCurrentIndex(m_computer->modifier(static_cast<int>(KeyboardModifier::Shift)));
  ui->comboCtrl->setCurrentIndex(m_computer->modifier(static_cast<int>(KeyboardModifier::Ctrl)));
  ui->comboAlt->setCurrentIndex(m_computer->modifier(static_cast<int>(KeyboardModifier::Alt)));
  ui->comboMeta->setCurrentIndex(m_computer->modifier(static_cast<int>(KeyboardModifier::Meta)));
  ui->comboSuper->setCurrentIndex(m_computer->modifier(static_cast<int>(KeyboardModifier::Super)));
  ui->comboAltGr->setCurrentIndex(m_computer->modifier(static_cast<int>(KeyboardModifier::AltGr)));

  ui->chkDeadTopLeft->setChecked(m_computer->switchCorner(static_cast<int>(TopLeft)));
  ui->chkDeadTopRight->setChecked(m_computer->switchCorner(static_cast<int>(TopRight)));
  ui->chkDeadBottomLeft->setChecked(m_computer->switchCorner(static_cast<int>(BottomLeft)));
  ui->chkDeadBottomRight->setChecked(m_computer->switchCorner(static_cast<int>(BottomRight)));
  ui->sbSwitchCornerSize->setValue(m_computer->switchCornerSize());

  ui->chkWeakX11Focus->setChecked(Settings::value(Settings::Computer::WeakX11Focus.arg(m_computer->name())).toBool());

  ui->chkFixCapsLock->setChecked(m_computer->fix(CapsLock));
  ui->chkFixNumLock->setChecked(m_computer->fix(NumLock));
  ui->chkFixScrollLock->setChecked(m_computer->fix(ScrollLock));
  ui->chkFixXTest->setChecked(m_computer->fix(XTest));

  // The size in pixels lines a client up with the server's monitors in the layout. A computer laid out by its
  // monitors takes its size from them.
  if (m_computer->monitors().isEmpty()) {
    auto *group = new QGroupBox(tr("Size in layout"), this);
    auto *row = new QHBoxLayout(group);
    m_width = new QSpinBox(group);
    m_height = new QSpinBox(group);
    const auto size = m_computer->geometry().isValid() ? m_computer->geometry().size() : ComputerList::kDefaultSize;
    m_width->setRange(1, 100000);
    m_height->setRange(1, 100000);
    m_width->setValue(size.width());
    m_height->setValue(size.height());
    m_width->setSuffix(tr(" px"));
    m_height->setSuffix(tr(" px"));
    m_width->setToolTip(tr("Use the computer's screen resolution to line it up pixel for pixel"));
    m_height->setToolTip(m_width->toolTip());
    row->addWidget(new QLabel(tr("Width"), group));
    row->addWidget(m_width);
    row->addWidget(new QLabel(tr("Height"), group));
    row->addWidget(m_height);
    row->addStretch();
    if (auto *mainLayout = qobject_cast<QBoxLayout *>(layout()); mainLayout != nullptr)
      mainLayout->insertWidget(1, group);
  }

  // Motion on a client is scaled by its mouse speed, e.g. to make up for its screen having smaller pixels than the
  // server's. Motion on the server itself isn't scaled.
  if (!m_computer->isServer()) {
    auto *group = new QGroupBox(tr("Mouse"), this);
    auto *row = new QHBoxLayout(group);
    m_mouseSpeed = new QSpinBox(group);
    m_mouseSpeed->setRange(10, 500);
    m_mouseSpeed->setValue(m_computer->mouseSpeed());
    m_mouseSpeed->setSuffix(QStringLiteral(" %"));
    m_mouseSpeed->setToolTip(
        tr("How far the cursor moves on this computer for the same mouse movement, compared with the server. "
           "Raise it if the cursor feels slower here, e.g. because this computer's screen has smaller pixels")
    );
    row->addWidget(new QLabel(tr("Speed"), group));
    row->addWidget(m_mouseSpeed);
    row->addStretch();
    if (auto *mainLayout = qobject_cast<QBoxLayout *>(layout()); mainLayout != nullptr)
      mainLayout->insertWidget(m_width != nullptr ? 2 : 1, group);
  }

  connect(ui->buttonBox, &QDialogButtonBox::accepted, this, &ComputerSettingsDialog::accept);
  connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &ComputerSettingsDialog::reject);
  connect(ui->btnAddAlias, &QPushButton::clicked, this, &ComputerSettingsDialog::addAlias);
  connect(ui->btnRemoveAlias, &QPushButton::clicked, this, &ComputerSettingsDialog::removeAlias);
  connect(ui->lineAddAlias, &QLineEdit::textChanged, this, &ComputerSettingsDialog::checkNewAliasName);
  connect(ui->listAliases, &QListWidget::itemSelectionChanged, this, &ComputerSettingsDialog::aliasSelected);
}

void ComputerSettingsDialog::accept()
{
  if (ui->lineNameEdit->text().isEmpty()) {
    QMessageBox::warning(
        this, tr("Computer name is empty"),
        tr("The computer name cannot be empty. "
           "Please either fill in a name or cancel the dialog.")
    );
    return;
  }
  if (!ui->lblNameError->text().isEmpty()) {
    return;
  }

  m_computer->setName(ui->lineNameEdit->text());

  m_computer->aliases().clear();

  for (int i = 0; i < ui->listAliases->count(); i++) {
    QString alias(ui->listAliases->item(i)->text());
    if (alias == ui->lineNameEdit->text()) {
      QMessageBox::warning(
          this, tr("Computer name matches alias"),
          tr("The computer name cannot be the same as an alias. "
             "Please either remove the alias or change the computer name.")
      );
      return;
    }
    if (!m_computer->aliases().contains(alias))
      m_computer->addAlias(alias);
  }

  m_computer->setModifier(KeyboardModifier::Shift, ui->comboShift->currentIndex());
  m_computer->setModifier(KeyboardModifier::Ctrl, ui->comboCtrl->currentIndex());
  m_computer->setModifier(KeyboardModifier::Alt, ui->comboAlt->currentIndex());
  m_computer->setModifier(KeyboardModifier::Meta, ui->comboMeta->currentIndex());
  m_computer->setModifier(KeyboardModifier::Super, ui->comboSuper->currentIndex());
  m_computer->setModifier(KeyboardModifier::AltGr, ui->comboAltGr->currentIndex());

  m_computer->setSwitchCorner(TopLeft, ui->chkDeadTopLeft->isChecked());
  m_computer->setSwitchCorner(TopRight, ui->chkDeadTopRight->isChecked());
  m_computer->setSwitchCorner(BottomLeft, ui->chkDeadBottomLeft->isChecked());
  m_computer->setSwitchCorner(BottomRight, ui->chkDeadBottomRight->isChecked());
  m_computer->setSwitchCornerSize(ui->sbSwitchCornerSize->value());
  if (m_mouseSpeed != nullptr)
    m_computer->setMouseSpeed(m_mouseSpeed->value());

  m_computer->setFix(CapsLock, ui->chkFixCapsLock->isChecked());
  m_computer->setFix(NumLock, ui->chkFixNumLock->isChecked());
  m_computer->setFix(ScrollLock, ui->chkFixScrollLock->isChecked());
  m_computer->setFix(XTest, ui->chkFixXTest->isChecked());

  Settings::setValue(Settings::Computer::WeakX11Focus.arg(m_computer->name()), ui->chkWeakX11Focus->isChecked());

  if (m_width != nullptr) {
    const QSize size(m_width->value(), m_height->value());
    m_computer->setGeometry(QRect(m_computer->geometry().isValid() ? m_computer->geometry().topLeft() : QPoint(), size));
  }

  QDialog::accept();
}

void ComputerSettingsDialog::addAlias()
{
  if (!ui->lineAddAlias->text().isEmpty() &&
      ui->listAliases->findItems(ui->lineAddAlias->text(), Qt::MatchFixedString).isEmpty()) {
    new QListWidgetItem(ui->lineAddAlias->text(), ui->listAliases);
    ui->lineAddAlias->clear();
  }
}

void ComputerSettingsDialog::removeAlias() const
{
  QList<QListWidgetItem *> items = ui->listAliases->selectedItems();
  qDeleteAll(items);
}

void ComputerSettingsDialog::checkNewAliasName(const QString &text)
{
  ui->btnAddAlias->setEnabled(!text.isEmpty() && ui->lblAliasError->text().isEmpty());
}

void ComputerSettingsDialog::aliasSelected()
{
  ui->btnRemoveAlias->setEnabled(!ui->listAliases->selectedItems().isEmpty());
}
