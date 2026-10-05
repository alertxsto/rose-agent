#pragma once
#include "AgentTypes.h"
#include <QDialog>
#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QPointer>
class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QNetworkReply;
namespace rose::agent {
class ProviderDialog final : public QDialog {
    Q_OBJECT
public:
    explicit ProviderDialog(QWidget *parent = nullptr);
    ~ProviderDialog() override;
private:
    void loadModels();
    void requestModels(QByteArray key);
    void connectKey();
    void saveSettings();
    void updateControls();
    void cancelPending();
    void showError(const AgentError &);
    QUrl baseUrl() const;
    ProviderConfig connection() const;
    void baseChanged();
    QComboBox *model_;
    QLineEdit *base_, *password_;
    QLabel *keyStatus_, *status_, *context_;
    QPushButton *connect_, *save_;
    QString reference_ = "kenari";
    QString preferredModel_ = "deepseek-v4-1-flash";
    ProviderConfig initial_;
    QNetworkAccessManager network_;
    QPointer<QNetworkReply> modelReply_;
    QPointer<QFutureWatcherBase> credentialWatcher_;
    QByteArray modelBytes_;
    bool credentialReadPending_ = false;
    bool keyAvailable_ = false;
};
} // namespace rose::agent
