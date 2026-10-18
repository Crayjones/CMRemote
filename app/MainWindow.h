#pragma once
#include "rust/cxx.h"
#include "remotemanager-core/src/bridge.rs.h"
#include <QHash>
#include <QJsonObject>
#include <QMainWindow>
#include <QPointer>
#include <QUrl>
#include <QRubberBand>
#include <QTreeWidgetItem>
#include "RdpTarget.h"
#include "Credentials.h"
#include "SshAuthentication.h"
#include "VncAuthentication.h"
#include "DocumentContext.h"
#include "ConnectionXml.h"
#include "ExternalTools.h"
#include "ApplicationPreferences.h"
#include "UpdateCheck.h"
#include <memory>
#include <functional>
namespace Ui { class MainWindow; }
class SessionPane;
class ClipboardEndpoint;
class SessionFullscreenController;
class PanelWindow;
class ProfileEditor;
class QDialog;
class QTabWidget;
class QMenu;
class QAction;
class QTreeWidgetItem;
class QTimer;
class SshWorker;
class VncWorker;
class SshHostKeyService;
class TerminalAppearanceService;
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const QString& dataPath, bool testMode = false, QWidget* parent = nullptr, std::shared_ptr<ClipboardEndpoint> clipboard = {}, QString sshHostKeyPath = {}, QString terminalAppearancePath = {}, const QString& preferencesPath = {});
    ~MainWindow() override;
    enum class DocumentState { Loading, Open, Locked, Error };
    Q_ENUM(DocumentState)
    DocumentState documentState() const { return documentState_; }
    bool documentProtected() const { return protectedDocument_; }
    enum TreeItemType { DocumentItem = QTreeWidgetItem::UserType + 1, ProfileItem };
    QString selectedId() const;
    bool documentRootSelected() const;
    bool inheritanceAvailable() const;
    QString lastError() const { return lastError_; }
    size_t sessionCount() const;
    // The window showing a session fullscreen, if any.
    QMainWindow* sessionFullscreenWindow() const;
    // Where Check for Updates asks for the latest release (tests use a local file).
    void setUpdateSource(const QUrl& source);
    // Packaged application only: check, download and install releases automatically.
    void enableAutomaticUpdates(const QString& dataDirectory);
    // Takes ownership; tests supply an updater with fixture releases and installation.
    void attachUpdater(class AutoUpdater* updater);
    DocumentId openConnectionFile(const QString& path, bool createNew = false);
    QList<DocumentId> connectionFiles() const { return documentOrder_; }
    DocumentId selectedDocument() const;
    bool selectConnectionFile(DocumentId id);
    bool selectConnectionProfile(ProfileReference profile);
    void unlockConnectionFile(DocumentId id, Secret master);
    void saveConnectionFile();
    void saveConnectionFileAs(const QString& path, bool overwriteConfirmed = false);
    void closeConnectionFile();
    // Import into the selected folder (or the file's top level); export the selected file.
    void importConnections(const QString& path);
    void importPuttySessions();
    void exportConnections(const QString& path, const ExportOptions& options);
public slots:
    void seedDemo();
    void retryDocumentUnlock();
    void addFolder();
    void addConnection();
    void saveSelected();
    void connectSelected(bool mock = false);
    void quickConnect(const QString& endpoint, bool mock = false);
    // Quick Connect protocol chosen from the RDP button's arrow menu ("rdp" or "ssh").
    QString quickConnectProtocol() const;
    void setQuickConnectProtocol(const QString& protocol);
    void saveAsConnection();
    void closeTab(int index);
    void disconnectCurrent();
    void reconnectCurrent();
    void deleteSelected();
    void addPanel();
    void closePanel(QWidget* panel);
signals:
    void documentReady();
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
private:
    void initializeFiles();
    std::shared_ptr<DocumentContext> context(DocumentId id) const;
    rm::Core& activeCore();
    rm::Core& sessionCore(DocumentId id);
    bool selectFileReference(const std::variant<DocumentRootReference, ProfileReference>& reference);
    void syncSelectedFile();
    void rebuildFiles();
    void fileReply(DocumentReply reply, QString profile = {}, std::function<void()> next = {}, bool automatic = false);
    void fileWarning(DocumentId id, std::function<void()> next);
    void writeFileProfile(rm::ProfileChange change, const QString& input, std::function<void()> next, bool inheritPassword = false,
                          std::shared_ptr<Secret> password = {}, std::optional<SessionTarget> account = {});
    void promptFileUnlock(DocumentId id, std::function<void()> next = {});
    void reviewFilePasswords(DocumentId id, std::function<void()> next = {});
    void processFileUnlocks();
    void finishFileUnlock(DocumentId id, bool unlocked);
    void repairFileIds(DocumentId id);
    void promptNewFileMaster(DocumentId id, std::function<void(std::optional<Secret>)> complete);
    void changeFileMaster(DocumentId id, std::function<void()> next = {});
    void protectFile(bool enabled);
    void passwordProtectFile(bool enabled);
    void savePasswordEdit(std::function<void()> next);
    void commitPasswordEdit(std::vector<PasswordAuthorization> reviewed, std::function<void()> next);
    void saveTemporaryToFile(QPointer<SessionPane> pane);
    void saveTargetToFile(const SessionTarget& target, std::shared_ptr<Secret> password = {});
    void updateFileActions();
    QString documentHelp() const;
    void revertEditor();
    void writeProfile(rm::ProfileChange change, const QString& input, std::function<void()> next = {});
    void guard(std::function<void()> next);
    void finishTransition();
    void saveDraft(std::function<void()> next);
    void queueAutoSave();
    void startAutoSave();
    void autoSaveFailed(const QString& error);
    void autoSaveFinished(bool canceled);
    void recoverSave(std::function<void()> next);
    void selectTree(const QString& id, bool document = false);
    void selectionChanged();
    void previewDraft();
    void connectCommitted(bool mock);
    void requestSession(const QString& profile, SessionTarget target, bool mock, QTabWidget* owner, SessionPane* replacement = nullptr, bool promptOnly = false, DocumentId document = {}, const QString& folder = {});
    // Connections a tree item opens: itself, or every connectable connection
    // beneath a folder in file order.
    QStringList connectableProfiles(DocumentId document, const QString& id, bool mock = false) const;
    void continueFolderConnect();
    void rebuildConnectionsMenu(QMenu* menu);
    struct FolderConnect { DocumentId document; QString folder; QStringList pending; QPointer<QTabWidget> owner; bool mock = false; };
    std::optional<FolderConnect> folderConnect_;
    struct CredentialAttempt {
        QString profile; SessionTarget target;
        DocumentId document;
        quint64 generation = 0;
        QByteArray revision;
        QPointer<QTabWidget> owner; QPointer<SessionPane> page;
        quint64 token; bool replacing = false;
        std::optional<TransientCredentials> credentials;
        std::optional<rust::Box<rm::ssh::Authentication>> sshAuthentication;
        std::optional<rust::Box<rm::vnc::Authentication>> vncAuthentication;
        std::optional<SessionTarget> authenticatedTarget;
        QString folder; // Folder-connect source for a missing username or password.
        bool console = false; // "Connect to console session" on a profile that does not request it.
        bool save = false; std::optional<Secret> savePassword; // Quick Connect "Save to Connections".
    };
    using Attempt = std::shared_ptr<CredentialAttempt>;
    std::optional<SessionTarget> committedTarget(const QString& profile) const;
    std::optional<SessionTarget> expectedTarget(const CredentialAttempt& attempt) const;
    bool validAttempt(const Attempt& attempt) const;
    void beginFileCredentials(DocumentId document, const QString& profile, SessionTarget target, QTabWidget* owner, SessionPane* replacement, bool promptOnly, const QString& folder = {});
    std::optional<SessionTarget> attemptTarget(const DocumentContext& doc, const QString& profile, const QString& folder) const;
    void lookupFileCredentials(Attempt attempt);
    void promptCredentials(Attempt attempt);
    void finishCredentials(Attempt attempt);
    void saveAttempt(const Attempt& attempt, const SessionTarget& target);
    void clearCredentialDialog();
    void connectDifferent();
    // Connection tree commands matching mRemoteNG's context menu.
    enum class ConnectOption { Console, Fullscreen, NoCredentials, ChoosePanel };
    void setupTreeActions();
    void connectWithOption(ConnectOption option);
    void connectInPanel(ProfileReference profile, QPointer<QTabWidget> owner, ConnectOption option);
    void choosePanel(std::function<void(QTabWidget*)> chosen);
    void disconnectSelected();
    bool profileInScope(DocumentId document, const QString& id, SessionPane* pane) const;
    void duplicateSelected();
    std::optional<QJsonObject> siblingMove(int delta) const;
    void moveSelected(int delta);
    void reconnectAll();
    void sendMultiSsh();
    void showTransferFile(ProfileReference profile);
    void showExternalTools();
    void runExternalTool(int index);
    void rebuildExternalTools();
    void showImportDialog();
    void showExportDialog();
    QString importDestination() const;
    void importXml(DocumentId owner, QString destination, QByteArray bytes, QString label);
    void finishImport(DocumentId owner, QString destination, ConnectionXml& xml, const Secret& key, QString label);
    void continueReconnectAll();
    void checkHostStatus();
    void attachSession(const QString& profile, SessionTarget target, bool mock, QPointer<QTabWidget> owner,
                       QPointer<SessionPane> replacement, std::optional<TransientCredentials> credentials, DocumentId document = {},
                       std::optional<rust::Box<rm::ssh::Authentication>> sshAuthentication = {},
                       std::optional<rust::Box<rm::vnc::Authentication>> vncAuthentication = {});
    void cancelCredentials();
    void disconnectPane(SessionPane* pane);
    bool subtreeHasTab(const QString& id) const;
    void deleteCommitted();
    void add(bool folder);
    void rebuild(const QString& select = {});
    void properties();
    void fail(const QString& message);
    void updateActions();
    void filterConnections();
    void sortConnections(Qt::SortOrder order);
    QTabWidget* currentSessions() const;
    QList<QTabWidget*> sessionPanels() const;
    QList<SessionPane*> sessionPages() const;
    QTabWidget* ownerOf(SessionPane* pane) const;
    void focusPane(SessionPane* pane);
    void configureTabs(QTabWidget* tabs);
    void closePane(SessionPane* pane);
    void closeAllSessions();
    void reconnectPane(QPointer<SessionPane> pane, bool promptOnly = false);
    void movePane(QPointer<SessionPane> pane, QPointer<QTabWidget> destination);
    void renamePage(QTabWidget* tabs, QWidget* page, bool panel);
    void resetLayout();
    void fullscreenPane(QPointer<SessionPane> pane);
    void returnFromFullscreen(QPointer<PanelWindow> window, QPointer<SessionPane> pane);
    bool isFullscreenWindow(const QWidget* window) const;
    // A fullscreen session's own window and the tab it returns to.
    struct FullscreenWindow { QPointer<PanelWindow> window; QPointer<QTabWidget> origin; int index = -1; };
    QList<FullscreenWindow> fullscreenWindows_;
    // Browser-style windows: every window has a panel bar (groups) whose panels
    // hold session tabs. The main window's groups is ui_->groupTabs.
    QList<QTabWidget*> groupBars() const;
    QTabWidget* groupsOf(QWidget* panel) const;
    QMainWindow* hostOf(QTabWidget* groups) const;
    PanelWindow* panelWindowOf(QWidget* widget) const;
    SessionFullscreenController* fullscreenFor(QWidget* widget) const;
    void exitFullscreenFor(SessionPane* pane);
    void configureGroups(QTabWidget* groups);
    QWidget* createPanel(QTabWidget* groups, const QString& name = {});
    void movePaneTo(SessionPane* pane, QTabWidget* destination, int index = -1);
    void movePanel(QWidget* panel, QTabWidget* destination, int index = -1);
    void closeEmptyPanel(QWidget* panel);
    PanelWindow* createPanelWindow(QPoint position, QSize size);
    void requestCloseWindow(PanelWindow* window);
    void retireWindow(PanelWindow* window);
    void lendDocks(PanelWindow* window, bool here);
    QList<QPointer<PanelWindow>> panelWindows_, retiredWindows_;
    QPointer<PanelWindow> lastWindow_, docksWindow_;
    int nextWindow_ = 2;
    // Tab/panel drag between bars and windows (Chrome-style tear-off).
    struct TabDrag {
        QPointer<QTabBar> bar; QPointer<QWidget> item; bool panel = false;
        bool detached = false, synthetic = false; QPointer<PanelWindow> floating; QPoint offset; QPointer<QWidget> sourcePanel;
    } drag_;
    struct DropTarget { QPointer<QTabWidget> groups, sessions; int index = -1; };
    DropTarget dropTarget(QPoint global, QWidget* exclude) const;
    // The same blue drop highlight the docks show, over where a dragged tab lands.
    void showDropHighlight(const DropTarget& target);
    QPointer<QRubberBand> dropHighlight_;
    bool tabDragEvent(QTabBar* bar, QEvent* event);
    void finishDrag(QPoint global);
    Qt::WindowStates applicationFullscreenState_;
    QRect applicationFullscreenGeometry_;
    void showOptions();
    void openSettings(int page);
    void applyPreferences(PreferenceValues values);
    PreferenceValues currentPreferences() const;
    // View > Panel Tabs shows or hides the General/Panel N row in every window.
    void applyPanelTabs();
    bool commandAsControl_ = false, autoUpdate_ = true;
    QString theme_ = "light", lightStyleSheet_;
    void applyThemeStyle();
    class AutoUpdater* updater_ = nullptr;
    QString promptedUpdate_;
    bool relaunchAfterUpdate_ = false, skipUpdateOnExit_ = false, updatePreferenceKnown_ = false, updateCheckRequested_ = false;
    void promptUpdateInstall(const QString& version, bool again);
    void finishUpdatePreparation(bool ok, const QString& error);
    void showUpdateCheck(bool ok, const Updates::Release& latest, const QString& error, bool installable);
    // macOS: Quick Connect sits in the title bar beside the window buttons.
    void placeQuickConnectInTitleBar();
    void fitTitleBar();
    bool titleBarDrag(QObject* watched, QEvent* event);
    bool quickConnectInTitleBar_ = false;
    QAction* titleBarInset_ = nullptr;
    int titleBarHeight_ = 0;
    void setConfigMode(bool defaults, bool inheritance);
    ConnectionDefaults defaults_;
    bool preferencesReady_ = false;
    ApplicationPreferences* preferences_ = nullptr;
    class OpenH264Codec* openh264_ = nullptr;
    bool openh264Enabled_ = true;
    QPointer<QDialog> optionsDialog_;
    enum class Notice { Info, Warning, Error };
    void notification(const QString& text, Notice level = Notice::Info);
    // "RDP alice@host:3389 (Office PC)" for session notifications.
    QString describeSession(const SessionPane* pane) const;
    void updateNotificationDetails();
    void configureMenus();
    void rebuildJumpMenu();
    QAction* menuAction(QMenu* menu, const char* name, bool enabled, std::function<void()> command = {});
    void popup(QMenu* menu, const QPoint& position);
    QMenu* treeMenu(const QString& id, bool document = false);
    QMenu* sessionMenu(SessionPane* pane);
    QMenu* groupMenu(QWidget* panel);
    QMenu* configMenu();
    QMenu* notificationMenu();
    void finishSshShutdown();
    void showTerminalAppearance();
    void checkForUpdates();
    class UpdateChecker* updateChecker_ = nullptr;
    TerminalAppearanceService* terminalAppearance_ = nullptr;
    bool appearanceStopped_ = true;
    SshHostKeyService* sshTrust_ = nullptr;
    QString sshHostKeyPath_;
    QSet<SshWorker*> sshWorkers_;
    QSet<VncWorker*> vncWorkers_;
    bool sshTrustStopped_ = true;
    std::shared_ptr<ClipboardEndpoint> clipboardEndpoint_;
    DocumentService* files_ = nullptr;
    QHash<DocumentId, std::shared_ptr<DocumentContext>> documents_;
    QList<DocumentId> documentOrder_;
    QHash<DocumentId, quint64> treeGenerations_;
    DocumentId editorDocument_;
    QHash<DocumentId, QPointer<QDialog>> fileDialogs_;
    QHash<DocumentId, std::shared_ptr<std::vector<std::function<void()>>>> fileReviews_;
    struct FileUnlock {
        quint64 generation;
        QByteArray revision;
        int submissions = 0;
        QString error;
        std::vector<std::function<void()>> continuations;
    };
    QHash<DocumentId, std::shared_ptr<FileUnlock>> fileUnlocks_;
    QList<DocumentId> unlockQueue_;
    DocumentId unlockingFile_;
    QAction* closeFileAction_ = nullptr;
    QAction* repairIdsAction_ = nullptr;
    struct PendingPassword {
        ProfileReference profile;
        PasswordEdit edit;
        std::optional<Secret> replacement;
    };
    std::optional<PendingPassword> pendingPassword_;
    ProfileEditor* editor_ = nullptr;
    std::unique_ptr<Ui::MainWindow> ui_;
    rust::Box<rm::Core> core_;
    QHash<QString, QJsonObject> profiles_;
    std::optional<Qt::SortOrder> treeOrder_;
    QList<QPointer<SessionPane>> reconnectQueue_; // Tools > Reconnect all, one credential prompt at a time.
    QList<ProfileReference> fullscreenOnOpen_;
    QList<ExternalTool> externalTools_;
    QString externalToolsPath_; // "Connect in fullscreen mode" requests awaiting their tab.
    QString dataPath_, lastError_;
    bool testMode_;
    bool transitionPending_ = false;
    bool closeApproved_ = false;
    bool autoSaveQueued_ = false, autoSaveInFlight_ = false, autoSaveError_ = false;
    std::function<void()> pendingTransition_;
    QPointer<QDialog> saveRecovery_;
    DocumentState documentState_ = DocumentState::Loading;
    bool documentExists_ = false, protectedDocument_ = false, closeRequested_ = false;
    Attempt credentialAttempt_;
    QPointer<QDialog> credentialDialog_;
    QPointer<SessionPane> credentialPage_;
    QPointer<QTabWidget> credentialOwner_;
    quint64 credentialToken_ = 0;
    int nextPanel_ = 1;
    QTimer* pollTimer_ = nullptr;
};
