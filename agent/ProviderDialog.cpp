#include "ProviderDialog.h"
#include "Credentials.h"
#include "ProviderSettings.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <QCryptographicHash>
#include <QHostAddress>
#include <QNetworkProxy>
#include <cmath>

namespace rose::agent {
namespace {
constexpr auto defaultBase = "https://kenari.id/v1";
constexpr int contextTokensRole = Qt::UserRole + 1;
constexpr qsizetype maximumModelBytes = 2 * 1024 * 1024;
struct CredentialKey {
    explicit CredentialKey(QByteArray bytes) : value(std::move(bytes)) {}
    CredentialKey(CredentialKey &&) = default;
    CredentialKey(const CredentialKey &) = delete;
    ~CredentialKey() { clear(); }
    void clear() { value.fill('\0'); value.clear(); }
    QByteArray value;
};
}
ProviderDialog::ProviderDialog(QWidget *parent) : QDialog(parent) {
    setObjectName("ProviderDialog"); setWindowTitle(tr("OpenAI-compatible provider"));
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout; layout->addLayout(form);
    base_ = new QLineEdit(defaultBase, this); base_->setObjectName("ProviderBaseUrl");
    base_->setPlaceholderText("https://provider.example/v1"); form->addRow(tr("Base URL"), base_);
    password_ = new QLineEdit(this); password_->setObjectName("ProviderPassword"); password_->setEchoMode(QLineEdit::Password);
    password_->setPlaceholderText(tr("API key — leave blank to reuse the connected key")); form->addRow(tr("API key"), password_);
    connect_ = new QPushButton(tr("Connect / load models"), this); connect_->setObjectName("ConnectProvider"); layout->addWidget(connect_);
    model_ = new QComboBox(this); model_->setObjectName("ProviderModel");
    model_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    model_->setMinimumContentsLength(28); form->addRow(tr("Model"), model_);
    context_ = new QLabel(this); context_->setObjectName("ProviderContext"); context_->setWordWrap(true); layout->addWidget(context_);
    keyStatus_ = new QLabel(this); keyStatus_->setObjectName("ProviderKeyStatus"); keyStatus_->setWordWrap(true); layout->addWidget(keyStatus_);
    auto *notice = new QLabel(tr("Models and context information come from the provider. Your prompt, photos and model context are sent to this Base URL. API keys stay in the OS keyring, never in Rose Agent settings."), this);
    notice->setWordWrap(true); layout->addWidget(notice);
    status_ = new QLabel(this); status_->setObjectName("ProviderStatus"); status_->setWordWrap(true); status_->setTextFormat(Qt::PlainText); layout->addWidget(status_);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    save_ = buttons->button(QDialogButtonBox::Save); save_->setText(tr("Use model")); save_->setObjectName("SaveProviderSettings");
    buttons->button(QDialogButtonBox::Cancel)->setObjectName("CancelProviderSettings"); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &ProviderDialog::saveSettings);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(this, &QDialog::finished, this, [this] { cancelPending(); });
    connect(connect_, &QPushButton::clicked, this, &ProviderDialog::connectKey);
    connect(password_, &QLineEdit::textChanged, this, [this] { updateControls(); });
    connect(model_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { updateControls(); });
    QSettings settings(QSettings::NativeFormat, QSettings::UserScope, "RoseAgent", "RoseAgent");
    const auto loaded = ProviderSettings::load(settings);
    if (auto *config = std::get_if<ProviderConfig>(&loaded)) {
        initial_ = *config; preferredModel_ = config->model;
        auto base = config->endpoint; auto path = base.path();
        if (path.endsWith("/chat/completions")) path.chop(QString("/chat/completions").size());
        base.setPath(path); base_->setText(base.toString());
    }
    connect(base_, &QLineEdit::textChanged, this, [this] { baseChanged(); });
    baseChanged(); resize(500, sizeHint().height());
    QTimer::singleShot(0, this, &ProviderDialog::loadModels);
}
ProviderDialog::~ProviderDialog() { cancelPending(); }
QUrl ProviderDialog::baseUrl() const {
    QUrl base(base_->text().trimmed(), QUrl::StrictMode);
    auto path = base.path(); while (path.endsWith('/')) path.chop(1); base.setPath(path);
    return base;
}
ProviderConfig ProviderDialog::connection() const {
    ProviderConfig config;
    config.endpoint = baseUrl(); config.endpoint.setPath(config.endpoint.path() + "/chat/completions");
    config.kind = config.endpoint.host() == "localhost" || QHostAddress(config.endpoint.host()).isLoopback() ? ProviderKind::Local : ProviderKind::Hosted;
    config.model = "catalog";
    return config;
}
void ProviderDialog::baseChanged() {
    cancelPending(); keyAvailable_ = false; model_->clear();
    const auto endpoint = connection().endpoint;
    if (endpoint == initial_.endpoint && !initial_.credentialReference.isEmpty()) reference_ = initial_.credentialReference;
    else if (baseUrl() == QUrl(defaultBase)) reference_ = "kenari";
    else reference_ = "openai." + QString::fromLatin1(QCryptographicHash::hash(baseUrl().toEncoded(), QCryptographicHash::Sha256).toHex());
    keyStatus_->setText(tr("Enter the API key once, or reuse the connected key for this Base URL."));
    status_->clear(); updateControls();
}
void ProviderDialog::updateControls() {
    const bool busy = credentialWatcher_ || modelReply_;
    base_->setEnabled(!busy); model_->setEnabled(!busy && model_->count());
    password_->setEnabled(!busy); connect_->setEnabled(!busy);
    save_->setEnabled(!busy && (keyAvailable_ || connection().kind == ProviderKind::Local) && model_->currentIndex() >= 0 && password_->text().isEmpty());
    const auto tokens = model_->currentData(contextTokensRole).toLongLong();
    context_->setText(tokens > 0 ? tr("Context window: %1 tokens (from provider)").arg(tokens) : tr("Context window: managed by provider; not advertised."));
}
void ProviderDialog::cancelPending() {
    password_->clear();
    if (credentialWatcher_ && credentialReadPending_) credentialWatcher_->cancel();
    credentialWatcher_ = nullptr; credentialReadPending_ = false;
    if (modelReply_) {
        auto *reply = modelReply_.data(); modelReply_ = nullptr;
        reply->disconnect(this); reply->abort(); reply->deleteLater();
    }
    modelBytes_.clear();
}
void ProviderDialog::showError(const AgentError &error) { status_->setText(errorName(error.code) + ": " + error.message); }
void ProviderDialog::loadModels() {
    if (credentialWatcher_ || modelReply_) return;
    if (auto error = validateConfig(connection())) { showError(*error); return; }
    status_->setText(tr("Loading provider models…"));
    auto *watcher = new QFutureWatcher<std::shared_ptr<CredentialLookup>>;
    credentialWatcher_ = watcher; credentialReadPending_ = true; updateControls();
    QPointer<ProviderDialog> self(this);
    connect(watcher, &QFutureWatcher<std::shared_ptr<CredentialLookup>>::finished, watcher, [self, watcher] {
        auto future = watcher->future(); const bool cancelled = future.isCanceled();
        auto result = future.resultCount() ? future.result()->take() : std::variant<QByteArray, AgentError>{AgentError{AgentErrorCode::Credentials, "Credential lookup cancelled."}};
        future = {}; watcher->deleteLater();
        auto *key = std::get_if<QByteArray>(&result);
        if (!self || cancelled || self->credentialWatcher_ != watcher) { if (key) key->fill('\0'); return; }
        self->credentialWatcher_ = nullptr; self->credentialReadPending_ = false;
        self->keyAvailable_ = key && !key->isEmpty();
        if (!self->keyAvailable_) {
            if (key) key->fill('\0');
            self->keyStatus_->setText(self->tr("Enter the API key for this Base URL, then Connect."));
            if (self->connection().kind == ProviderKind::Local) { self->requestModels({}); return; }
            self->status_->setText(std::holds_alternative<AgentError>(result) ? std::get<AgentError>(result).message : self->tr("The stored API key is empty."));
            self->updateControls(); return;
        }
        self->keyStatus_->setText(self->tr("API key connected — stored securely in the OS keyring."));
        self->requestModels(std::move(*key));
    });
    watcher->setFuture(Credentials::readAsync(reference_));
}
void ProviderDialog::requestModels(QByteArray key) {
    auto endpoint = baseUrl(); endpoint.setPath(endpoint.path() + "/models");
    if (endpoint.host() == "localhost") endpoint.setHost("127.0.0.1");
    network_.setProxy(connection().kind == ProviderKind::Local ? QNetworkProxy::NoProxy : QNetworkProxy::DefaultProxy);
    QNetworkRequest request{endpoint};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Accept", "application/json");
    if (!key.isEmpty()) request.setRawHeader("Authorization", "Bearer " + key);
    key.fill('\0'); key.clear();
    auto *reply = network_.get(request); modelReply_ = reply; modelBytes_.clear(); updateControls();
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (modelReply_ != reply) return;
        modelBytes_.append(reply->read(maximumModelBytes - modelBytes_.size() + 1));
        if (modelBytes_.size() > maximumModelBytes) {
            cancelPending(); showError({AgentErrorCode::Protocol, tr("Provider model list exceeds the supported size.")}); updateControls();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (modelReply_ != reply) return;
        modelReply_ = nullptr; reply->deleteLater();
        const auto http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || http != 200) {
            if (http == 401 || http == 403) keyAvailable_ = false;
            modelBytes_.clear(); showError({http == 401 || http == 403 ? AgentErrorCode::Authentication : AgentErrorCode::Network,
                tr("Cannot load models (HTTP %1). Check the Base URL, API key or connection.").arg(http)}); updateControls(); return;
        }
        QJsonParseError error; const auto document = QJsonDocument::fromJson(modelBytes_, &error); modelBytes_.clear();
        if (error.error != QJsonParseError::NoError || !document.isObject() || !document.object().value("data").isArray()) {
            showError({AgentErrorCode::Protocol, tr("The provider returned an invalid model list.")}); updateControls(); return;
        }
        struct Model { QString id; bool free; qint64 tokens; }; QVector<Model> models; QSet<QString> seen;
        for (const auto &value : document.object().value("data").toArray()) {
            const auto model = value.toObject(); const auto id = model.value("id").toString();
            const auto inputs = model.value("modalities").toObject().value("input").toArray();
            if (id.isEmpty() || id != id.trimmed() || seen.contains(id)
                || (model.value("tool_call").isBool() && !model.value("tool_call").toBool())
                || (model.value("endpoints").isArray() && !model.value("endpoints").toArray().contains("chat"))
                || (model.value("modalities").isObject() && (!inputs.contains("text") || !inputs.contains("image")))) continue;
            const auto context = model.contains("context_length") ? model.value("context_length") : model.value("context_window");
            const double advertised = context.toDouble();
            const qint64 tokens = context.isDouble() && std::isfinite(advertised) && advertised > 0 && advertised <= 9007199254740991.0 && std::floor(advertised) == advertised ? qint64(advertised) : 0;
            seen.insert(id); models.append({id, model.value("pricing").toObject().value("free").toBool(), tokens});
        }
        std::sort(models.begin(),models.end(),[](const auto &a,const auto &b){return a.free != b.free ? a.free : a.id < b.id;});
        const auto selected = model_->currentData().toString(); model_->clear();
        for (const auto &model : models) {
            model_->addItem(model.id + (model.free ? tr(" — free") : QString{}), model.id);
            model_->setItemData(model_->count()-1, model.tokens, contextTokensRole);
        }
        int index = model_->findData(selected.isEmpty() ? preferredModel_ : selected);
        if (index >= 0) model_->setCurrentIndex(index);
        if (models.isEmpty()) showError({AgentErrorCode::Protocol, tr("No usable chat models were advertised. The agent needs tools and photos; capabilities may be unknown when the provider does not advertise them.")});
        else status_->setText(tr("Choose a model, then Use model. Endpoint and other settings are automatic."));
        updateControls();
    });
    QTimer::singleShot(30000, reply, [this, reply] {
        if (modelReply_ != reply) return;
        cancelPending(); showError({AgentErrorCode::Timeout, tr("Loading models timed out. Connect again to retry.")}); updateControls();
    });
}
void ProviderDialog::connectKey() {
    if (credentialWatcher_ || modelReply_) return;
    if (auto error = validateConfig(connection())) { showError(*error); return; }
    if (password_->text().trimmed().isEmpty()) { loadModels(); return; }
    auto key = std::make_shared<CredentialKey>(password_->text().trimmed().toUtf8()); password_->clear();
    auto *watcher = new QFutureWatcher<std::optional<AgentError>>;
    credentialWatcher_ = watcher; credentialReadPending_ = false; updateControls();
    status_->setText(tr("Connecting API key… This writes to the OS keyring; closing does not roll back an already queued write."));
    QPointer<ProviderDialog> self(this); const auto reference = reference_;
    connect(watcher, &QFutureWatcher<std::optional<AgentError>>::finished, watcher, [self, watcher] {
        auto future = watcher->future(); const auto error = future.result(); future = {}; watcher->deleteLater();
        if (!self || self->credentialWatcher_ != watcher) return;
        self->credentialWatcher_ = nullptr;
        if (error) { self->showError(*error); self->updateControls(); return; }
        self->loadModels();
    });
    watcher->setFuture(QtConcurrent::run([reference, key]() {
        const auto error = Credentials::store(reference, key->value); key->clear(); return error;
    }));
}
void ProviderDialog::saveSettings() {
    if (credentialWatcher_ || modelReply_ || (!keyAvailable_ && connection().kind != ProviderKind::Local) || model_->currentIndex() < 0 || !password_->text().isEmpty()) return;
    auto config = connection(); config.model = model_->currentData().toString();
    config.credentialReference = keyAvailable_ ? reference_ : QString{};
    config.contextWindowTokens = model_->currentData(contextTokensRole).toLongLong();
    // Physical request-memory bound, not an estimated or user-chosen token window.
    config.maxContextBytes = 64 * 1024 * 1024;
    QSettings settings(QSettings::NativeFormat, QSettings::UserScope, "RoseAgent", "RoseAgent");
    if (auto error = ProviderSettings::store(settings, config)) { showError(*error); return; }
    accept();
}
} // namespace rose::agent
