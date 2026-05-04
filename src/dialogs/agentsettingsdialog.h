/*
 * Copyright (c) 2026 Shotcut contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef AGENTSETTINGSDIALOG_H
#define AGENTSETTINGSDIALOG_H

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QSpinBox;

class AgentSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AgentSettingsDialog(QWidget *parent = nullptr);

protected:
    void accept() override;

private slots:
    void onAllowRemoteToggled(bool checked);
    void onCopyUrl();
    void onGenerateToken();

private:
    void updateUrlPreview();

    QCheckBox *m_enabled;
    QSpinBox *m_port;
    QComboBox *m_bind;
    QLineEdit *m_token;
    QCheckBox *m_allowRemote;
    QLabel *m_urlPreview;
    QDialogButtonBox *m_buttons;
};

#endif // AGENTSETTINGSDIALOG_H
