#include "ModuleManagerDialog.h"

#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QStyle>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

// 构造函数一次性创建表格；运行期间只更新状态单元格和按钮可用性，
// 不重新解析配置或重建行。

namespace qframework
{
namespace
{
// 判断模块是否提供可以显示的界面按钮。
bool isUiType(ModuleType type)
{
    // 两种 UI 类型都提供“显示模块界面”按钮。
    return type == ModuleType::InProcessUi || type == ModuleType::ProcessUi;
}

// 判断模块是否由 ProcessSupervisor 管理，只有这类模块允许重启。
bool isProcessType(ModuleType type)
{
    // 只有子进程类型可以通过监督器执行 restart。
    return type == ModuleType::ProcessUi || type == ModuleType::ProcessNonUi;
}
}

// 构造一次性表格和按钮；之后只接受状态/可用性更新，不重新创建行。
ModuleManagerDialog::ModuleManagerDialog(const QVector<ModuleConfig>& modules,
                                         QWidget* parent)
    : QDialog(parent),
      tableWidget_(new QTableWidget(modules.size(), 4, this))
{
    // 表格禁止用户直接编辑，所有变化来自框架状态信号。
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

    // 按配置顺序创建行，并把 moduleId 保存到按钮动态属性。
    int row = 0;
    for (const ModuleConfig& module : modules) {
        rows_.insert(module.id, row);
        moduleStates_.insert(
            module.id,
            module.enabled ? QString::fromUtf8(u8"等待启动") : QString::fromUtf8(u8"已禁用"));
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
            // showButton 初始禁用，直到对应 UI 对象/窗口句柄准备完成。
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
            // 被禁用的配置不能重启；运行中或失败的子进程均可请求重启。
            QToolButton* restartButton = new QToolButton(actionWidget);
            QIcon icon = QIcon::fromTheme(QStringLiteral("view-refresh"));
            if (icon.isNull())
                icon = style()->standardIcon(QStyle::SP_BrowserReload);
            restartButton->setIcon(icon);
            restartButton->setToolTip(QString::fromUtf8(u8"重新启动子进程"));
            restartButton->setAutoRaise(true);
            restartButton->setFixedSize(28, 28);
            restartButton->setProperty("moduleId", module.id);
            restartButton->setProperty("configuredEnabled", module.enabled);
            restartButton->setEnabled(module.enabled);
            connect(restartButton,
                    &QToolButton::clicked,
                    this,
                    &ModuleManagerDialog::onRestartButtonClicked);
            actionLayout->addWidget(restartButton);
            restartButtons_.insert(module.id, restartButton);
            restartBusy_.insert(module.id, false);
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

// 按 moduleId 更新状态单元格和详细 tooltip，未知 ID 安全忽略。
void ModuleManagerDialog::setModuleState(const QString& moduleId,
                                         const QString& state,
                                         const QString& detail)
{
    // 未知 ID 可能来自已移除配置，安全忽略而不是越界访问表格。
    const int row = rows_.value(moduleId, -1);
    if (row < 0)
        return;
    QTableWidgetItem* item = tableWidget_->item(row, 2);
    if (item == nullptr)
        return;
    item->setText(state);
    item->setToolTip(detail);
    moduleStates_.insert(moduleId, state);
    updateRestartButton(moduleId);
}

// 切换对应“显示”按钮；非 UI 模块没有按钮且无需处理。
void ModuleManagerDialog::setUiAvailable(const QString& moduleId, bool available)
{
    // 非 UI 模块不在 showButtons_ 中，因此没有按钮可更新。
    QToolButton* button = showButtons_.value(moduleId, nullptr);
    if (button != nullptr)
        button->setEnabled(available);
}

// operationBusyChanged 与状态文本共同决定按钮；任一仍在进行都不能重复提交。
void ModuleManagerDialog::setRestartBusy(const QString& moduleId, bool busy)
{
    if (!restartButtons_.contains(moduleId))
        return;
    restartBusy_.insert(moduleId, busy);
    updateRestartButton(moduleId);
}

// 从点击按钮的动态属性取出 moduleId，再发出纯请求信号。
void ModuleManagerDialog::onShowButtonClicked()
{
    // sender 是实际点击的 QToolButton，动态属性保存其 moduleId。
    QObject* button = sender();
    if (button != nullptr)
        emit showModuleRequested(button->property("moduleId").toString());
}

// 发出重启请求，不在表格槽中同步等待子进程，避免阻塞 UI。
void ModuleManagerDialog::onRestartButtonClicked()
{
    // 只发请求，不在对话框线程同步等待子进程停止/启动。
    QObject* button = sender();
    if (button != nullptr)
        emit restartModuleRequested(button->property("moduleId").toString());
}

void ModuleManagerDialog::updateRestartButton(const QString& moduleId)
{
    QToolButton* button = restartButtons_.value(moduleId, nullptr);
    if (button == nullptr)
        return;
    const QString state = moduleStates_.value(moduleId);
    const bool lifecycleBusy = state == QStringLiteral("Starting") ||
                               state == QStringLiteral("Stopping") ||
                               state == QStringLiteral("Restarting");
    button->setEnabled(button->property("configuredEnabled").toBool() &&
                       !restartBusy_.value(moduleId, false) &&
                       !lifecycleBusy);
}

// 把枚举转换成和 INI 相同的稳定文本，便于用户对照配置。
QString ModuleManagerDialog::moduleTypeText(ModuleType type) const
{
    // 使用与 INI 相同的稳定类型文本，便于初学者对照配置。
    if (type == ModuleType::InProcessUi)
        return QStringLiteral("InProcessUi");
    if (type == ModuleType::InProcessNonUi)
        return QStringLiteral("InProcessNonUi");
    if (type == ModuleType::ProcessUi)
        return QStringLiteral("ProcessUi");
    return QStringLiteral("ProcessNonUi");
}
}
