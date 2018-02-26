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

// This class is because I can't convince invokeMethod to call directly
// to mainwindow without crashing. So I do it to this object which lives
// on the main thread, and it updates the UI accordingly.
class FileListThunk : public QObject
{
    Q_OBJECT

public:
    FileListThunk(Ui::MainWindow* _main_window_ptr)
    {
        qDebug() << _main_window_ptr;
        main_window_ptr = _main_window_ptr;
    }

private:
    Ui::MainWindow* main_window_ptr;

public slots:
    void update_file_list(void* new_file_list);
};

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

    void retrieve_files();
    void download_file_to(QString depot_file, QString dest_dir);

};


class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = 0);
    ~MainWindow();

    QProcess tunnel_process;

private:
    Ui::MainWindow *ui;
    FileListThunk file_list_thunker;

    bool ConnectTunnel;
    SSHTunnel* Tunnel;
    QThread TunnelThread;

    virtual void closeEvent(QCloseEvent *event);

public slots:
    void on_pushButton_clicked();

    void on_treeWidget_itemDoubleClicked(QTreeWidgetItem* item, int column);

    void ShowContextMenu(const QPoint& point);
    void TestAction();
};

#endif // MAINWINDOW_H
