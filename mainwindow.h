#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <qprocess.h>
#include <qthread.h>
#include <qdebug.h>
#include <qtreeview.h>
#include <qtreewidget.h>
#include <qdialog.h>
#include <qplaintextedit.h>

namespace Ui {
class MainWindow;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
struct FileEntry
{
    // depot_file is empty if local only
    QString depot_file;

    // file on disc
    QString local_file;

    // if local != head revision, we can't operate on the file at ALL
    // other than to subscribe or unsubscribe.
    // if head revision is zero, its a local only file that needs to be added.
    int head_revision;

    // if we have a local revision, then its a subscribed file
    // otherwise its just a downloadable file.
    int local_revision;

    // state of the file in the depot.
    bool open_for_edit;
    bool open_by_another;

    bool exists_in_depot;

    FileEntry()
    {
        exists_in_depot = false;
        head_revision = 0;
        local_revision = 0;
        open_by_another = false;
        open_for_edit = false;
    }
};


//-----------------------------------------------------------------------------
// This class is because I can't convince invokeMethod to call directly
// to mainwindow without crashing. So I do it to this object which lives
// on the main thread, and it updates the UI accordingly.
//-----------------------------------------------------------------------------
class FileListThunk : public QObject
{
    Q_OBJECT

public:
    FileListThunk(class MainWindow* _thunk_to)
    {
        thunk_to = _thunk_to;
    }

private:
    class MainWindow* thunk_to;

public slots:
    void RefreshUIThunk(void* new_file_list, void* new_server_changes);
    void PostFileHistoryThunk(QString file, void* file_history);
};

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
class SSHTunnel : public QObject
{
    Q_OBJECT

public:
    SSHTunnel(FileListThunk* _thunk);

    QProcess P;
private:
    FileListThunk* thunk;
    bool retrying; // to prevent infinite recursion
    void run_ssh();

public slots:
    //void thread_fn();
    void shutdown_fn();
    void shutdown_tunnel();
    void echo_std();
    void echo_err();
    void tunnel_closed(int exit_code, QProcess::ExitStatus exit_status);

    bool run_perforce_command(QStringList command_and_args, QString const& std_input, QStringList& output_lines);

    QMap<QString, FileEntry>* retrieve_files(bool post_to_foreground);
    void download_file_to(QString depot_file, QString dest_dir);

    void get_file_history(QString depot_file);

    void RunQueue();
};

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
class SubmitDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SubmitDialog(const QStringList& SubmitFiles, QWidget* parent = nullptr);

    QPlainTextEdit* commit_msg_edit;

};

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    QProcess tunnel_process;

    void RefreshUI(QMap<QString, FileEntry>* NewFileMap, QStringList* NewServerChanges);
    void PostFileHistory(QString file, QStringList* history);

private:
    Ui::MainWindow *ui;
    FileListThunk file_list_thunker;

    bool ConnectTunnel;
    SSHTunnel* Tunnel;
    QThread TunnelThread;

    QMap<QString, FileEntry>* FileMap;
    QStringList* ServerChanges;

    QAction* Action_Edit;
    QAction* Action_Revert;
    QAction* Action_AddToDepot;
    QAction* Action_DeleteDisc;
    QAction* Action_DeleteDepot;
    QAction* Action_Subscribe;
    QAction* Action_Unsubscribe;
    QAction* Action_Download;
    QAction* Action_Sync;
    QAction* Action_ForceSync;
    QAction* Action_ShowInExplorer;

    QAction* Action_RemoveFromQueue;

    virtual void closeEvent(QCloseEvent *event);

public slots:
    void filter_changed(const QString& text);

    void on_btnRefresh_clicked();
    void on_btnRunQueue_clicked();
    void on_btnCommitSelected_clicked();
    void on_btnGetLatest_clicked();
    void on_btnOpenP4_clicked();

    void on_treeWidget_itemDoubleClicked(QTreeWidgetItem* item, int column);

    void on_lstEdited_itemSelectionChanged();

    void TreeSelectionChanged(const QItemSelection &, const QItemSelection &);

    void ShowContextMenu(const QPoint& point);

    void Context_Edit();
    void Context_Revert();
    void Context_AddToDepot();
    void Context_DeleteDisc();
    void Context_DeleteDepot();
    void Context_Subscribe();
    void Context_Unsubscribe();
    void Context_Download();
    void Context_Sync();
    void Context_ForceSync();
    void Context_ShowInExplorer();

    void ShowQueueContextMenu(const QPoint& point);

    void Context_RemoveFromQueue();
};

#endif // MAINWINDOW_H
