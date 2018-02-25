#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <qprocess.h>

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = 0);
    ~MainWindow();

    QProcess tunnel_process;

private:
    Ui::MainWindow *ui;

public slots:
    void on_pushButton_clicked();

    void ShowContextMenu(const QPoint& point);
    void TestAction();
};

#endif // MAINWINDOW_H
