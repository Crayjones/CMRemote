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
#include "OpenFileList.h"
#include "UpdateCheck.h"
#include <memory>
#include <functional>
struct ShellUi;
class SessionPane;
class ClipboardEndpoint;
class SessionFullscreenController;
class FullscreenWindow;
namespace ads { class CDockWidget; class CDockAreaWidget; class CFloatingDockContainer; }
class ProfileEditor;
class QDialog;
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
    // Why this copy cannot install updates when it has no updater (shown by Check for Updates).
    void setUpdatesUnavailable(const QString& reason) { updatesUnavailable_ = reason; }
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
    // Closes a tab of the current panel.
    void closeTab(int index);
    void disconnectCurrent();
    void reconnectCurrent();
    void deleteSelected();
    // View > New Panel: moves the current tab into a new panel beside its own.
    void addPanel();
signals:
    void documentReady();
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
private:
    // A panel: a Qt ADS dock area of session tabs.
    using Panel = QPointer<ads::CDockAreaWidget>;
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
    void requestSession(const QString& profile, SessionTarget target, bool mock, Panel owner, SessionPane* replacement = nullptr, bool promptOnly = false, DocumentId document = {}, const QString& folder = {});
    // Connections a tree item opens: itself, or every connectable connection
    // beneath a folder in file order.
    QStringList connectableProfiles(DocumentId document, const QString& id, bool mock = false) const;
    void continueFolderConnect();
    void rebuildConnectionsMenu(QMenu* menu);
    struct FolderConnect { DocumentId document; QString folder; QStringList pending; Panel owner; bool mock = false; };
    std::optional<FolderConnect> folderConnect_;
    struct CredentialAttempt {
        QString profile; SessionTarget target;
        DocumentId document;
        quint64 generation = 0;
        QByteArray revision;
        Panel owner; QPointer<SessionPane> page;
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
    void beginFileCredentials(DocumentId document, const QString& profile, SessionTarget target, Panel owner, SessionPane* replacement, bool promptOnly, const QString& folder = {});
    std::optional<SessionTarget> attemptTarget(const DocumentContext& doc, const QString& profile, const QString& folder) const;
    void lookupFileCredentials(Attempt attempt);
    void connectSshKey(Attempt attempt);
    void promptCredentials(Attempt attempt);
    void finishCredentials(Attempt attempt);
    void saveAttempt(const Attempt& attempt, const SessionTarget& target);
    void clearCredentialDialog();
    void connectDifferent();
    // Connection tree commands matching mRemoteNG's context menu.
    enum class ConnectOption { Console, Fullscreen, NoCredentials, ChoosePanel };
    void setupTreeActions();
    void connectWithOption(ConnectOption option);
    void connectInPanel(ProfileReference profile, Panel owner, ConnectOption option);
    // Chosen panel, or null with newPanel set for a new panel beside the current one.
    void choosePanel(std::function<void(Panel, bool newPanel)> chosen);
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
    void attachSession(const QString& profile, SessionTarget target, bool mock, Panel owner,
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
    // Every session is a Qt ADS dock tab; a panel is a dock area of session
    // tabs, in the main window or in a floating window of its own.
    QList<ads::CDockWidget*> sessionDocks() const;
    QList<SessionPane*> sessionPages() const;
    static ads::CDockWidget* dockOf(const SessionPane* pane);
    static SessionPane* paneOf(const ads::CDockWidget* dock);
    ads::CDockAreaWidget* panelOf(const SessionPane* pane) const;
    bool isOpenPane(const SessionPane* pane) const;
    // Panels holding sessions: the main window's first, then floating windows'.
    QList<ads::CDockAreaWidget*> sessionPanels() const;
    QList<SessionPane*> panelPanes(ads::CDockAreaWidget* panel) const;
    QString panelName(ads::CDockAreaWidget* panel) const;
    // Where new sessions open: the panel used last, else General.
    ads::CDockAreaWidget* currentPanel() const;
    SessionPane* currentPane() const;
    void focusPane(SessionPane* pane);
    void focusPanel(ads::CDockAreaWidget* panel);
    void configureSessionDock(ads::CDockWidget* dock);
    void closePane(SessionPane* pane);
    void closeAllSessions();
    void reconnectPane(QPointer<SessionPane> pane, bool promptOnly = false);
    void movePaneTo(SessionPane* pane, ads::CDockAreaWidget* destination, int index = -1);
    void splitPane(SessionPane* pane, int side);
    void floatPane(SessionPane* pane);
    void renamePane(SessionPane* pane);
    void resetLayout();
    void fullscreenPane(QPointer<SessionPane> pane);
    void returnFromFullscreen(QPointer<FullscreenWindow> window, QPointer<SessionPane> pane);
    bool isFullscreenWindow(const QWidget* window) const;
    FullscreenWindow* fullscreenWindowOf(const SessionPane* pane) const;
    void exitFullscreenFor(SessionPane* pane);
    // A fullscreen session's own window and the panel position it returns to.
    struct FullscreenEntry { QPointer<FullscreenWindow> window; QPointer<ads::CDockAreaWidget> origin; int index = -1; };
    QList<FullscreenEntry> fullscreenWindows_;
    // Every fullscreen window still alive, including those leaving fullscreen.
    QList<QPointer<FullscreenWindow>> ownedFullscreen_;
    void requestCloseFloating(ads::CFloatingDockContainer* window);
    // Keeps a window that showed sessions alive until those sessions are gone.
    void retireWindow(QWidget* window);
    QList<QPointer<QWidget>> retiredWindows_;
    // Shows the General placeholder while the main window has no session and
    // marks panels for theme.qss; coalesced after Qt ADS layout changes.
    void sessionLayoutChanged();
    void updateSessionLayout();
    bool sessionLayoutQueued_ = false;
    QPointer<ads::CDockWidget> lastSession_;
    int nextSessionDock_ = 1;
    mutable int nextPanel_ = 1;
    Qt::WindowStates applicationFullscreenState_;
    QRect applicationFullscreenGeometry_;
    void showOptions();
    void openSettings(int page);
    void applyPreferences(PreferenceValues values);
    PreferenceValues currentPreferences() const;
    bool commandAsControl_ = false, autoUpdate_ = true;
    QString theme_ = "light";
    class AutoUpdater* updater_ = nullptr;
    QString promptedUpdate_, downloadingUpdate_, updatesUnavailable_; // downloadingUpdate_: Check for Updates waits to offer it
    bool relaunchAfterUpdate_ = false, skipUpdateOnExit_ = false, updatePreferenceKnown_ = false, updateCheckRequested_ = false;
    void promptUpdateInstall(const QString& version, bool again);
    void showUpdateProgress();
    void finishUpdatePreparation(bool ok, const QString& error);
    void showUpdateCheck(bool ok, const Updates::Release& latest, const QString& error, bool installable);
    void showUpdateMessage(const QString& name, const QString& text, const QString& version);
    // macOS: Quick Connect sits in the title bar beside the window buttons.
    void placeQuickConnectInTitleBar();
    // Windows/Linux: the menus live in a movable toolbar (mRemoteNG's menu strip).
    void placeMenuBarInToolbar();
    QList<QToolBar*> shellToolbars() const;
    QToolBar* menuToolbar_ = nullptr;
    void fitTitleBar();
    bool titleBarDrag(QObject* watched, QEvent* event);
    bool quickConnectInTitleBar_ = false;
    QAction* titleBarInset_ = nullptr;
    int titleBarHeight_ = 0;
    void setConfigMode(bool defaults, bool inheritance);
    ConnectionDefaults defaults_;
    bool clipboardDefault_ = true;
    void applyClipboardDefault();
    bool preferencesReady_ = false;
    ApplicationPreferences* preferences_ = nullptr;
    OpenFileList* openFiles_ = nullptr;
    void restoreOpenFiles(const QStringList& paths);
    void rememberOpenFiles();
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
    void rebuildPanelsMenu();
    QAction* menuAction(QMenu* menu, const char* name, bool enabled, std::function<void()> command = {});
    void popup(QMenu* menu, const QPoint& position);
    QMenu* treeMenu(const QString& id, bool document = false);
    QMenu* sessionMenu(SessionPane* pane);
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
    std::unique_ptr<ShellUi> ui_;
    rust::Box<rm::Core> core_;
    QHash<QString, QJsonObject> profiles_;
    std::optional<Qt::SortOrder> treeOrder_;
    QList<QPointer<SessionPane>> reconnectQueue_; // Tools > Reconnect all, one credential prompt at a time.
    QList<ProfileReference> fullscreenOnOpen_, newPanelOnOpen_;
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
    quint64 credentialToken_ = 0;
    QTimer* pollTimer_ = nullptr;
};
