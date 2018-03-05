#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <qprocess.h>
#include <qthread.h>
#include <qdebug.h>
#include <qtreeview.h>
#include <qtreewidget.h>

namespace Ui {
class MainWindow;
}

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
    void RefreshUIThunk(void* new_file_list);
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

    bool run_perforce_command(QStringList command_and_args, QStringList& output_lines);

    void retrieve_files();
    void download_file_to(QString depot_file, QString dest_dir);

    void CheckoutFile(QString depot_file);
    void RevertFile(QString depot_file);
};


struct FileEntry
{

    QString depot_file;
    QString local_file;
    int head_revision;
    int local_revision;
    bool subscribed;
    bool open_for_edit;
    bool open_for_add;
    bool open_by_another;

    FileEntry()
    {
        head_revision = 0;
        local_revision = 0;
        subscribed = false;
        open_by_another = false;
        open_for_add = false;
        open_for_edit = false;
    }
};


class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = 0);
    ~MainWindow();

    QProcess tunnel_process;

    void RefreshUI(QMap<QString, FileEntry>* NewFileMap);


private:
    Ui::MainWindow *ui;
    FileListThunk file_list_thunker;

    bool ConnectTunnel;
    SSHTunnel* Tunnel;
    QThread TunnelThread;

    QMap<QString, FileEntry>* FileMap;

    QAction* Action_Edit;
    QAction* Action_Revert;

    virtual void closeEvent(QCloseEvent *event);

public slots:
    void on_btnConnect_clicked();

    void on_treeWidget_itemDoubleClicked(QTreeWidgetItem* item, int column);

    void ShowContextMenu(const QPoint& point);
    void TestAction();
    void Context_Edit();
    void Context_Revert();
};

#endif // MAINWINDOW_H
