#include "AgentDock.h"
#include "ProviderDialog.h"
#include "ProviderSettings.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStandardItemModel>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

namespace rose::agent {
namespace {
QString diagnosticText(const Diagnostic &d) {
    const auto severity = d.severity == Severity::Error ? QStringLiteral("Error") : d.severity == Severity::Warning ? QStringLiteral("Warning") : QStringLiteral("Information");
    return severity + " — " + d.code + ": " + d.message + "\n  object=" + d.objectId + " file=" + d.file
        + " span=" + QString::number(d.span.offset) + ":" + QString::number(d.span.length);
}
QStandardItem *item(const QString &text) {
    auto *result = new QStandardItem(text); result->setEditable(false); result->setToolTip(text); return result;
}
QLabel *plainLabel(const QString &text, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText); label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    label->setSizePolicy(policy);
    return label;
}
}
AgentDock::AgentDock(desktop::WorkspaceController *controller, QWidget *parent)
    : QDockWidget(tr("Agent chat"), parent), controller_(controller), run_(new AgentRun(controller, this)) {
    setObjectName("AgentDock"); setMinimumWidth(390);
    auto *content = new QWidget(this); setWidget(content);
    auto *layout = new QVBoxLayout(content); layout->setContentsMargins(10, 8, 10, 8); layout->setSpacing(6);
    workspace_ = plainLabel({}, content); workspace_->setObjectName("AgentWorkspaceSummary");
    auto headingFont = workspace_->font(); headingFont.setBold(true); workspace_->setFont(headingFont);
    layout->addWidget(workspace_);
    auto *header = new QHBoxLayout; header->setSpacing(5); layout->addLayout(header);
    provider_ = plainLabel({}, content); provider_->setObjectName("AgentProviderSummary"); header->addWidget(provider_, 1);
    provider_->setWordWrap(false);
    auto button = [](const QString &text, const QString &name, QWidget *owner, QHBoxLayout *row) {
        auto *b = new QPushButton(text, owner); b->setObjectName(name); row->addWidget(b); return b;
    };
    new_ = button(tr("New"), "AgentNewModel", content, header);
    open_ = button(tr("Open"), "AgentOpenModel", content, header);
    configure_ = button(tr("Provider…"), "AgentConfigureProvider", content, header);
    connect(new_, &QPushButton::clicked, this, &AgentDock::newModelRequested);
    connect(open_, &QPushButton::clicked, this, &AgentDock::openModelRequested);
    connect(configure_, &QPushButton::clicked, this, &AgentDock::configureProvider);

    transcript_ = new QScrollArea(content); transcript_->setObjectName("AgentTranscript");
    transcript_->setWidgetResizable(true); transcript_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    transcript_->setFrameShape(QFrame::NoFrame); transcript_->setMinimumHeight(60); layout->addWidget(transcript_, 1);
    messages_ = new QWidget(transcript_); transcript_->setWidget(messages_);
    messageLayout_ = new QVBoxLayout(messages_); messageLayout_->setContentsMargins(0, 6, 0, 6); messageLayout_->setSpacing(10);
    empty_ = plainLabel({}, messages_); empty_->setObjectName("AgentEmptyState"); empty_->setAlignment(Qt::AlignCenter);
    messageLayout_->addWidget(empty_); messageLayout_->addStretch();
    auto *chatBar = transcript_->verticalScrollBar();
    connect(chatBar, &QScrollBar::rangeChanged, this, [this, chatBar](int, int maximum) {
        if (followTail_) chatBar->setValue(maximum);
    });
    connect(chatBar, &QScrollBar::actionTriggered, this, [this, chatBar](int) {
        followTail_ = chatBar->sliderPosition() >= chatBar->maximum() - 32;
    });
    connect(chatBar, &QScrollBar::sliderMoved, this, [this, chatBar](int position) {
        followTail_ = position >= chatBar->maximum() - 32;
    });

    reviewCard_ = new QFrame(content); reviewCard_->setObjectName("AgentReviewCard");
    auto *reviewLayout = new QVBoxLayout(reviewCard_); reviewLayout->setContentsMargins(8, 6, 8, 6); reviewLayout->setSpacing(4);
    reviewSummary_ = plainLabel({}, reviewCard_); reviewSummary_->setObjectName("AgentReviewSummary"); reviewLayout->addWidget(reviewSummary_);
    auto *reviewActions = new QHBoxLayout; reviewLayout->addLayout(reviewActions);
    auto *detailsButton = button(tr("Review details…"), "AgentReviewDetails", reviewCard_, reviewActions);
    apply_ = button(tr("Apply"), "AgentApply", reviewCard_, reviewActions);
    reject_ = button(tr("Reject"), "AgentReject", reviewCard_, reviewActions);
    layout->addWidget(reviewCard_); reviewCard_->hide();
    details_ = new QDialog(this); details_->setObjectName("AgentReviewDialog"); details_->setWindowTitle(tr("Exact proposal review")); details_->resize(800, 560);
    auto *detailsLayout = new QVBoxLayout(details_);
    tuple_ = plainLabel(tr("No proposal to review."), details_); tuple_->setObjectName("AgentProposalTuple"); detailsLayout->addWidget(tuple_);
    diff_ = new QTableView(details_); diff_->setObjectName("AgentProposalDiff"); diffModel_ = new QStandardItemModel(this);
    diffModel_->setHorizontalHeaderLabels({tr("Change"), tr("Object"), tr("Field"), tr("Before"), tr("After")});
    diff_->setModel(diffModel_); diff_->setEditTriggers(QAbstractItemView::NoEditTriggers); diff_->setWordWrap(true);
    diff_->horizontalHeader()->setStretchLastSection(true); detailsLayout->addWidget(diff_, 1);
    diagnostics_ = new QPlainTextEdit(details_); diagnostics_->setObjectName("AgentProposalDiagnostics"); diagnostics_->setReadOnly(true);
    diagnostics_->setMaximumHeight(150); detailsLayout->addWidget(diagnostics_);
    detailsLayout->addWidget(plainLabel(tr("Apply changes only in memory, bound to this exact ID, revision and digest. Save / Save As remains a separate native operation."), details_));
    auto *closeDetails = new QDialogButtonBox(QDialogButtonBox::Close, details_); detailsLayout->addWidget(closeDetails);
    connect(closeDetails, &QDialogButtonBox::rejected, details_, &QDialog::reject);
    connect(detailsButton, &QPushButton::clicked, details_, &QDialog::exec);
    connect(apply_, &QPushButton::clicked, this, [this] {
        if (displayed_ && !fileBusy_ && writable_) {
            const Approval exact = *displayed_;
            if (!run_->applyReview(exact)) addMessage(tr("Status"), tr("This exact approval is no longer current. No changes were approved; send a fresh request."));
            updateControls();
        }
    });
    connect(reject_, &QPushButton::clicked, this, [this] {
        if (displayed_ && !run_->rejectReview(displayed_->id)) addMessage(tr("Status"), tr("This proposal is no longer current."));
        updateControls();
    });

    auto *statusRow = new QHBoxLayout; layout->addLayout(statusRow);
    status_ = plainLabel(tr("Ready · changes require review; saving is separate."), content); status_->setObjectName("AgentStatus"); statusRow->addWidget(status_, 1);
    auto *activityButton = button(tr("Activity…"), "AgentActivity", content, statusRow);
    clear_ = button(tr("Clear chat"), "AgentClearChat", content, statusRow);
    activityDialog_ = new QDialog(this); activityDialog_->setObjectName("AgentActivityDialog"); activityDialog_->setWindowTitle(tr("Run activity")); activityDialog_->resize(600, 360);
    auto *activityLayout = new QVBoxLayout(activityDialog_);
    activity_ = new QPlainTextEdit(activityDialog_); activity_->setObjectName("AgentActivityLog"); activity_->setReadOnly(true); activity_->setMaximumBlockCount(1000); activityLayout->addWidget(activity_);
    auto *closeActivity = new QDialogButtonBox(QDialogButtonBox::Close, activityDialog_); activityLayout->addWidget(closeActivity);
    connect(closeActivity, &QDialogButtonBox::rejected, activityDialog_, &QDialog::reject);
    connect(activityButton, &QPushButton::clicked, activityDialog_, &QDialog::exec);
    connect(clear_, &QPushButton::clicked, this, &AgentDock::clearConversation);

    attachments_ = new QListWidget(content); attachments_->setObjectName("AgentImages"); attachments_->setMaximumHeight(64); layout->addWidget(attachments_);
    connect(attachments_, &QListWidget::currentRowChanged, this, [this] { updateControls(); });
    prompt_ = new QPlainTextEdit(content); prompt_->setObjectName("AgentPrompt");
    prompt_->setPlaceholderText(tr("Ask about this model or describe a change…\nEnter to send · Shift+Enter for a new line · images welcome"));
    prompt_->setMinimumHeight(64); prompt_->setMaximumHeight(96); prompt_->installEventFilter(this); layout->addWidget(prompt_);
    auto *composerActions = new QHBoxLayout; composerActions->setSpacing(5); layout->addLayout(composerActions);
    attach_ = button(tr("Attach image…"), "AgentAttachImage", content, composerActions);
    remove_ = button(tr("Remove"), "AgentRemoveImage", content, composerActions); composerActions->addStretch();
    cancel_ = button(tr("Stop"), "AgentCancel", content, composerActions);
    start_ = button(tr("Send"), "AgentStart", content, composerActions);
    start_->setDefault(false);
    connect(attach_, &QPushButton::clicked, this, &AgentDock::attachImage);
    connect(remove_, &QPushButton::clicked, this, &AgentDock::removeImage);
    connect(start_, &QPushButton::clicked, this, &AgentDock::start);
    connect(cancel_, &QPushButton::clicked, run_, &AgentRun::cancel);
    connect(prompt_, &QPlainTextEdit::textChanged, this, [this] { updateControls(); });
    connect(run_, &AgentRun::event, this, &AgentDock::receive);
    connect(controller_, &desktop::WorkspaceController::completed, this, [this](quint64, const WorkspaceReply &) { updateControls(); });
    connect(controller_, &desktop::WorkspaceController::workspaceChanged, this, [this](quint64 generation) {
        if (workspaceGeneration_ != generation) {
            workspaceGeneration_ = generation; selected_.clear(); clearConversation();
            prompt_->clear(); images_.clear(); attachments_->clear();
            status_->setText(tr("Workspace changed · a fresh conversation is ready."));
        }
        updateControls();
    });
    workspaceGeneration_ = controller_->sessionGeneration(); refreshProvider(); updateControls();
}
bool AgentDock::eventFilter(QObject *object, QEvent *event) {
    if (object == prompt_ && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && !(key->modifiers() & Qt::ShiftModifier)) {
            if (start_->isEnabled()) start();
            return true;
        }
    }
    return QDockWidget::eventFilter(object, event);
}
void AgentDock::setWorkspaceState(bool fileBusy, bool writable) { fileBusy_ = fileBusy; writable_ = writable; updateControls(); }
void AgentDock::setSelectedElements(QVector<ElementId> selected) { selected_ = std::move(selected); }
bool AgentDock::busy() const { return run_->active() || run_->status() == RunStatus::AwaitingReview; }
void AgentDock::updateControls() {
    const bool idle = !busy() && !fileBusy_;
    const bool hasWorkspace = controller_->hasWorkspace();
    workspace_->setText(hasWorkspace ? tr("Workspace · revision %1%2").arg(controller_->revision()).arg(writable_ ? QString() : tr(" · read-only")) : tr("No model open"));
    empty_->setText(hasWorkspace ? tr("What would you like to design?\nAsk a question, describe a model change, or attach a diagram.\nProposed changes are always yours to review.")
        : tr("Start with a model\nCreate a new native model or open a .mdl, then ask here.\nManual editing and saving work offline."));
    new_->setEnabled(idle); open_->setEnabled(idle); configure_->setEnabled(idle); clear_->setEnabled(idle);
    prompt_->setReadOnly(fileBusy_); attach_->setEnabled(idle && images_.size() < 4);
    attachments_->setVisible(!images_.isEmpty()); remove_->setVisible(!images_.isEmpty());
    remove_->setEnabled(idle && attachments_->currentRow() >= 0);
    start_->setEnabled(idle && hasWorkspace && (!prompt_->toPlainText().trimmed().isEmpty() || !images_.isEmpty()));
    cancel_->setVisible(busy()); cancel_->setEnabled(busy() && run_->status() != RunStatus::Applying);
    const bool awaiting = run_->status() == RunStatus::AwaitingReview && displayed_ && run_->review()
        && displayed_->id == run_->review()->id && displayed_->baseRevision == run_->review()->baseRevision && displayed_->digest == run_->review()->digest;
    apply_->setEnabled(awaiting && run_->canApply() && !fileBusy_ && writable_);
    reject_->setEnabled(awaiting && !fileBusy_);
}
void AgentDock::refreshProvider() {
    QSettings settings(QSettings::NativeFormat, QSettings::UserScope, "RoseAgent", "RoseAgent");
    const auto loaded = ProviderSettings::load(settings);
    if (auto *config = std::get_if<ProviderConfig>(&loaded)) provider_->setText(config->endpoint.host() == "kenari.id" ? tr("Kenari · %1").arg(config->model) : config->model);
    else provider_->setText(tr("Set up a provider"));
    provider_->setAccessibleDescription(provider_->text());
}
void AgentDock::configureProvider() {
    ProviderDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) refreshProvider();
}
void AgentDock::attachImage() {
    const auto path = QFileDialog::getOpenFileName(this, tr("Attach diagram or photo"), {}, tr("Images (*.png *.jpg *.jpeg *.webp);;All files (*)"));
    if (path.isEmpty()) return;
    auto loaded = loadImage(path);
    if (auto *error = std::get_if<AgentError>(&loaded)) {
        status_->setText(tr("Attachment · %1").arg(errorName(error->code)));
        addMessage(tr("Status"), error->message); return;
    }
    auto image = std::get<AgentImage>(std::move(loaded));
    attachments_->addItem(QFileInfo(path).fileName() + " · " + image.mimeType + " · " + QString::number(image.bytes.size() / 1024) + tr(" KiB"));
    images_.append(std::move(image)); attachments_->setCurrentRow(attachments_->count() - 1); updateControls();
}
void AgentDock::removeImage() {
    const auto row = attachments_->currentRow();
    if (row < 0 || row >= images_.size()) return;
    images_.removeAt(row); delete attachments_->takeItem(row); updateControls();
}
void AgentDock::clearConversation() {
    run_->resetConversation();
    while (messageLayout_->count() > 2) {
        auto *entry = messageLayout_->takeAt(1); delete entry->widget(); delete entry;
    }
    empty_->show(); displayed_.reset(); reviewCard_->hide(); details_->reject();
    activity_->clear(); diagnostics_->clear(); diffModel_->removeRows(0, diffModel_->rowCount());
    tuple_->setText(tr("No proposal to review."));
    status_->setText(tr("Chat cleared · the model is unchanged.")); updateControls();
}
void AgentDock::addMessage(const QString &role, const QString &text, bool user) {
    if (user) followTail_ = true;
    empty_->hide();
    auto *row = new QWidget(messages_); auto *rowLayout = new QHBoxLayout(row); rowLayout->setContentsMargins(0, 0, 0, 0);
    auto *bubble = new QFrame(row); bubble->setObjectName(user ? "AgentUserMessage" : role == tr("Assistant") ? "AgentAssistantMessage" : "AgentStatusMessage");
    bubble->setFrameShape(QFrame::StyledPanel); bubble->setAutoFillBackground(true);
    bubble->setBackgroundRole(user ? QPalette::AlternateBase : QPalette::Base);
    auto *bubbleLayout = new QVBoxLayout(bubble); bubbleLayout->setContentsMargins(10, 7, 10, 8); bubbleLayout->setSpacing(4);
    auto *speaker = plainLabel(role, bubble); auto font = speaker->font(); font.setBold(true); speaker->setFont(font); bubbleLayout->addWidget(speaker);
    auto *body = plainLabel(text, bubble); body->setObjectName(user ? "AgentUserText" : "AgentMessageText"); bubbleLayout->addWidget(body);
    if (user) rowLayout->addSpacing(24);
    rowLayout->addWidget(bubble, 1);
    if (!user) rowLayout->addSpacing(18);
    messageLayout_->insertWidget(messageLayout_->count() - 1, row);
    if (followTail_) QTimer::singleShot(0, this, [this] {
        if (followTail_) transcript_->verticalScrollBar()->setValue(transcript_->verticalScrollBar()->maximum());
    });
}
void AgentDock::start() {
    if (!start_->isEnabled()) return;
    QSettings settings(QSettings::NativeFormat, QSettings::UserScope, "RoseAgent", "RoseAgent");
    const auto loaded = ProviderSettings::load(settings);
    if (auto *error = std::get_if<AgentError>(&loaded)) { status_->setText(error->message); configureProvider(); return; }
    const AgentInput input{prompt_->toPlainText(), images_};
    QString text = input.prompt;
    for (int i = 0; i < attachments_->count(); ++i) text += (text.isEmpty() ? QString() : QStringLiteral("\n")) + tr("Attached: %1").arg(attachments_->item(i)->text());
    addMessage(tr("You"), text, true);
    displayed_.reset(); sequence_ = 0; lastStatus_ = RunStatus::Planning; reviewCard_->hide();
    prompt_->clear(); images_.clear(); attachments_->clear();
    run_->start(std::get<ProviderConfig>(loaded), input, selected_); updateControls();
}
void AgentDock::display(const Proposal &proposal) {
    displayed_ = Approval{proposal.id, proposal.baseRevision, proposal.digest};
    tuple_->setText(tr("Proposal ID: %1\nBase revision: %2\nDigest: %3").arg(proposal.id.value).arg(proposal.baseRevision).arg(QString::fromLatin1(proposal.digest.toHex())));
    diffModel_->removeRows(0, diffModel_->rowCount());
    for (const auto &d : proposal.diff) diffModel_->appendRow({item(d.kind), item(d.objectId), item(d.field), item(d.before), item(d.after)});
    diff_->resizeColumnsToContents(); diff_->resizeRowsToContents(); diagnostics_->clear();
    if (proposal.diagnostics.isEmpty()) diagnostics_->setPlainText(tr("No proposal diagnostics."));
    else for (const auto &d : proposal.diagnostics) diagnostics_->appendPlainText(diagnosticText(d));
    if (!proposal.newIds.isEmpty()) {
        diagnostics_->appendPlainText(tr("Exact proposed client-ID mappings:"));
        for (auto it = proposal.newIds.cbegin(); it != proposal.newIds.cend(); ++it) diagnostics_->appendPlainText(it.key() + " → " + it.value());
    }
    reviewSummary_->setText(tr("Proposal ready · %1 changes\nReview details before Apply. Not applied or saved.").arg(proposal.diff.size()));
    reviewCard_->show();
}
void AgentDock::receive(const RunEvent &event) {
    if (event.runId != run_->id() || event.sequence <= sequence_) return;
    sequence_ = event.sequence;
    status_->setText(event.error ? statusName(event.status) + " · " + errorName(event.error->code) : statusName(event.status));
    if (!event.activity.isEmpty()) activity_->appendPlainText(event.activity);
    if (event.assistantMessage) addMessage(tr("Assistant"), event.assistantMessage->text);
    if (event.status == RunStatus::AwaitingReview && event.proposal && (!displayed_ || displayed_->id != event.proposal->id)) display(*event.proposal);
    const bool outcome = event.status == RunStatus::Completed || event.status == RunStatus::Cancelled || event.status == RunStatus::Failed;
    if (outcome && event.status != lastStatus_) {
        addMessage(tr("Status"), event.error ? errorName(event.error->code) + ": " + event.error->message : event.activity);
        if (displayed_) reviewSummary_->setText(tr("Proposal inactive · %1\nDetails remain available; no further approval is allowed.").arg(statusName(event.status)));
    }
    lastStatus_ = event.status; updateControls(); emit workflowStateChanged();
}
} // namespace rose::agent
