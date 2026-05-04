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

#include "agentsettingsdialog.h"

#include "settings.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSpinBox>
#include <QVBoxLayout>

AgentSettingsDialog::AgentSettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("AI Agent Settings"));

    auto *layout = new QVBoxLayout(this);

    auto *intro = new QLabel(tr("The AI agent server exposes a JSON-RPC over WebSocket interface "
                                "that lets external tools query and edit your project. It is off "
                                "by default and binds to loopback only unless you explicitly "
                                "enable remote connections."));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *form = new QFormLayout;
    layout->addLayout(form);

    m_enabled = new QCheckBox(tr("Enable agent server on next start"));
    m_enabled->setChecked(Settings.agentServerEnabled());
    form->addRow(m_enabled);

    m_port = new QSpinBox;
    m_port->setRange(1, 65535);
    m_port->setValue(Settings.agentServerPort());
    form->addRow(tr("Port"), m_port);

    m_bind = new QComboBox;
    m_bind->setEditable(true);
    m_bind->addItem(QStringLiteral("127.0.0.1"));
    m_bind->addItem(QStringLiteral("::1"));
    m_bind->addItem(QStringLiteral("0.0.0.0"));
    m_bind->setCurrentText(Settings.agentServerBind());
    form->addRow(tr("Bind address"), m_bind);

    auto *tokenRow = new QHBoxLayout;
    m_token = new QLineEdit(Settings.agentServerToken());
    m_token->setEchoMode(QLineEdit::PasswordEchoOnEdit);
    auto *generate = new QPushButton(tr("Generate"));
    tokenRow->addWidget(m_token);
    tokenRow->addWidget(generate);
    auto *tokenContainer = new QWidget;
    tokenContainer->setLayout(tokenRow);
    form->addRow(tr("Bearer token"), tokenContainer);

    m_allowRemote = new QCheckBox(tr("Allow remote connections (insecure)"));
    m_allowRemote->setChecked(Settings.agentServerAllowRemote());
    form->addRow(m_allowRemote);

    m_urlPreview = new QLabel;
    m_urlPreview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Connection URL"), m_urlPreview);

    auto *copy = new QPushButton(tr("Copy connection URL"));
    layout->addWidget(copy);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(m_buttons);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &AgentSettingsDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &AgentSettingsDialog::reject);

    connect(generate, &QPushButton::clicked, this, &AgentSettingsDialog::onGenerateToken);
    connect(copy, &QPushButton::clicked, this, &AgentSettingsDialog::onCopyUrl);
    connect(m_allowRemote, &QCheckBox::toggled, this, &AgentSettingsDialog::onAllowRemoteToggled);

    connect(m_port, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) {
        updateUrlPreview();
    });
    connect(m_bind, &QComboBox::currentTextChanged, this, [this](const QString &) {
        updateUrlPreview();
    });

    updateUrlPreview();
}

void AgentSettingsDialog::onAllowRemoteToggled(bool checked)
{
    if (checked && m_token->text().isEmpty()) {
        QMessageBox::warning(this,
                             tr("AI Agent Settings"),
                             tr("Remote connections require a bearer token. "
                                "Please generate or enter one before enabling "
                                "remote access."));
    }
}

void AgentSettingsDialog::onGenerateToken()
{
    // 32 hex chars (128 bits of entropy) is plenty for a localhost-or-LAN
    // shared secret.
    QString token;
    token.reserve(32);
    auto *rng = QRandomGenerator::system();
    for (int i = 0; i < 32; ++i)
        token.append(QString::number(rng->bounded(16), 16));
    m_token->setText(token);
}

void AgentSettingsDialog::onCopyUrl()
{
    QApplication::clipboard()->setText(m_urlPreview->text());
}

void AgentSettingsDialog::updateUrlPreview()
{
    QString host = m_bind->currentText();
    if (host == QStringLiteral("0.0.0.0"))
        host = QStringLiteral("127.0.0.1");
    if (host.contains(':') && !host.startsWith('['))
        host = QStringLiteral("[%1]").arg(host);
    m_urlPreview->setText(QStringLiteral("ws://%1:%2/").arg(host).arg(m_port->value()));
}

void AgentSettingsDialog::accept()
{
    if (m_allowRemote->isChecked() && m_token->text().isEmpty()) {
        QMessageBox::warning(this,
                             tr("AI Agent Settings"),
                             tr("A bearer token is required when remote "
                                "connections are allowed."));
        return;
    }
    Settings.setAgentServerEnabled(m_enabled->isChecked());
    Settings.setAgentServerPort(m_port->value());
    Settings.setAgentServerBind(m_bind->currentText());
    Settings.setAgentServerToken(m_token->text());
    Settings.setAgentServerAllowRemote(m_allowRemote->isChecked());
    QDialog::accept();
}
