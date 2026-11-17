/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QSpinBox;

/**
 * The Audio tab of the settings dialog.
 */
class AudioSettingsWidget : public QWidget
{
  Q_OBJECT
public:
  explicit AudioSettingsWidget(QWidget *parent = nullptr);

  void load();
  void save() const;
  bool isModified() const;
  bool isDefault() const;
  void resetToDefault();
  void retranslate();

Q_SIGNALS:
  void changed();

private:
  void updateEnabled();
  static void fillDevices(QComboBox *combo, const QStringList &names, const QString &current);

  QGroupBox *m_clientGroup = nullptr;
  QCheckBox *m_forward = nullptr;
  QCheckBox *m_keepLocal = nullptr;
  QCheckBox *m_remoteMicrophone = nullptr;
  QLabel *m_driverNote = nullptr;

  QGroupBox *m_serverGroup = nullptr;
  QLabel *m_mixModeLabel = nullptr;
  QComboBox *m_mixMode = nullptr;
  QLabel *m_outputLabel = nullptr;
  QComboBox *m_output = nullptr;
  QLabel *m_inputLabel = nullptr;
  QComboBox *m_input = nullptr;
  QLabel *m_serverNote = nullptr;

  QLabel *m_portLabel = nullptr;
  QSpinBox *m_port = nullptr;
  QLabel *m_note = nullptr;
};
