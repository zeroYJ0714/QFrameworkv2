#include "ModuleManagerDialog.h"

#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QStyle>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace qframework
{
namespace
{
bool isUiType(ModuleType type)
{
    return type == ModuleType::InProcessUi || type == ModuleType::ProcessUi;
}

bool isProcessType(ModuleType type)
{
    return type == ModuleType::ProcessUi || type == ModuleType::ProcessNonUi;
}
}

ModuleManagerDialog::ModuleManagerDialog(const QVector<ModuleConfig>& modules,
                                         QWidget* parent)
    : QDialog(parent),
      tableWidget_(new QTableWidget(modules.size(), 4, this))
{
    setWindowTitle(QString::fromUtf8(u8"模块管理"));
    setModal(false);
    resize(760, 360);

    tableWidget_->setHorizontalHeaderLabels(
        QStringList() << QString::fromUtf8(u8"模块")
                      << QString::fromUtf8(u8"类型")
                      << QString::fromUtf8(u8"状态")
                      << QString::fromUtf8(u8"操作"));
    tableWidget_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableWidget_->setSelectionMode(QAbstractItemView::SingleSelection);
    tableWidget_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tableWidget_->verticalHeader()->setVisible(false);
    tableWidget_->horizontalHeader()->setStretchLastSection(false);
    tableWidget_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    tableWidget_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tableWidget_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    tableWidget_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

    int row = 0;
    for (const ModuleConfig& module : modules) {
        rows_.insert(module.id, row);
        QTableWidgetItem* nameItem = new QTableWidgetItem(
            module.displayName.isEmpty() ? module.id : module.displayName);
        nameItem->setToolTip(module.id);
        tableWidget_->setItem(row, 0, nameItem);
        tableWidget_->setItem(row, 1, new QTableWidgetItem(moduleTypeText(module.type)));
        tableWidget_->setItem(
            row,
            2,
            new QTableWidgetItem(module.enabled
                ? QString::fromUtf8(u8"等待启动") : QString::fromUtf8(u8"已禁用")));

        QWidget* actionWidget = new QWidget(tableWidget_);
        QHBoxLayout* actionLayout = new QHBoxLayout(actionWidget);
        actionLayout->setContentsMargins(4, 1, 4, 1);
        actionLayout->setSpacing(2);

        if (isUiType(module.type)) {
            QToolButton* showButton = new QToolButton(actionWidget);
            QIcon icon = QIcon::fromTheme(QStringLiteral("view-visible"));
            if (icon.isNull())
                icon = style()->standardIcon(QStyle::SP_DialogOpenButton);
            showButton->setIcon(icon);
            showButton->setToolTip(QString::fromUtf8(u8"显示模块界面"));
            showButton->setAutoRaise(true);
            showButton->setFixedSize(28, 28);
            showButton->setProperty("moduleId", module.id);
            showButton->setEnabled(false);
            connect(showButton,
                    &QToolButton::clicked,
                    this,
                    &ModuleManagerDialog::onShowButtonClicked);
            actionLayout->addWidget(showButton);
            showButtons_.insert(module.id, showButton);
        }
        if (isProcessType(module.type)) {
            QToolButton* restartButton = new QToolButton(actionWidget);
            QIcon icon = QIcon::fromTheme(QStringLiteral("view-refresh"));
            if (icon.isNull())
                icon = style()->standardIcon(QStyle::SP_BrowserReload);
            restartButton->setIcon(icon);
            restartButton->setToolTip(QString::fromUtf8(u8"重新启动子进程"));
            restartButton->setAutoRaise(true);
            restartButton->setFixedSize(28, 28);
            restartButton->setProperty("moduleId", module.id);
            restartButton->setEnabled(module.enabled);
            connect(restartButton,
                    &QToolButton::clicked,
                    this,
                    &ModuleManagerDialog::onRestartButtonClicked);
            actionLayout->addWidget(restartButton);
        }
        actionLayout->addStretch();
        tableWidget_->setCellWidget(row, 3, actionWidget);
        tableWidget_->setRowHeight(row, 34);
        ++row;
    }

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addWidget(tableWidget_);
}

void ModuleManagerDialog::setModuleState(const QString& moduleId,
                                         const QString& state,
                                         const QString& detail)
{
    const int row = rows_.value(moduleId, -1);
    if (row < 0)
        return;
    QTableWidgetItem* item = tableWidget_->item(row, 2);
    if (item == nullptr)
        return;
    item->setText(state);
    item->setToolTip(detail);
}

void ModuleManagerDialog::setUiAvailable(const QString& moduleId, bool available)
{
    QToolButton* button = showButtons_.value(moduleId, nullptr);
    if (button != nullptr)
        button->setEnabled(available);
}

void ModuleManagerDialog::onShowButtonClicked()
{
    QObject* button = sender();
    if (button != nullptr)
        emit showModuleRequested(button->property("moduleId").toString());
}

void ModuleManagerDialog::onRestartButtonClicked()
{
    QObject* button = sender();
    if (button != nullptr)
        emit restartModuleRequested(button->property("moduleId").toString());
}

QString ModuleManagerDialog::moduleTypeText(ModuleType type) const
{
    if (type == ModuleType::InProcessUi)
        return QStringLiteral("InProcessUi");
    if (type == ModuleType::InProcessNonUi)
        return QStringLiteral("InProcessNonUi");
    if (type == ModuleType::ProcessUi)
        return QStringLiteral("ProcessUi");
    return QStringLiteral("ProcessNonUi");
}
}
