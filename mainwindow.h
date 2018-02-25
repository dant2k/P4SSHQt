#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <qprocess.h>

namespace Ui {
class MainWindow;
}


class SSHTunnel : public QObject
{
    Q_OBJECT

public:
    SSHTunnel();

    QProcess P;
private:
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

    bool ConnectTunnel;
    SSHTunnel* Tunnel;
    QThread TunnelThread;

    virtual void closeEvent(QCloseEvent *event);

public slots:
    void on_pushButton_clicked();

    void ShowContextMenu(const QPoint& point);
    void TestAction();
};

#endif // MAINWINDOW_H
