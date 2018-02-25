#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "qfileiconprovider.h"
#include "qprocess.h"
#include "qdebug.h"
#include "qdiriterator.h"
#include "qhash.h"
#include "qmenu.h"


SSHTunnel::SSHTunnel() : P(this)
{
    connect(&P, &QProcess::readyReadStandardError, this, &SSHTunnel::echo_err);
    connect(&P, &QProcess::readyReadStandardOutput, this, &SSHTunnel::echo_std);
    connect(&P, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &SSHTunnel::tunnel_closed);
}

void SSHTunnel::tunnel_closed(int, QProcess::ExitStatus)
{
    qDebug() << "SSL Tunnel Closed";
}

void SSHTunnel::run_ssh()
{
    QStringList list;
    list << "-N" << "-L" << "1234:localhost:1666" << "-i" << TunnelKey << TunnelUser << "@" << TunnelServer;

    qDebug() << "Running SSH";

    P.start("ssh", list);
    P.waitForStarted();

    // The tunnel is connecting in the background, however there's no signal
    // for us to know once it's done connecting, so we just sleep 5 seconds.
    qDebug() << "Waiting for connection...";
    QThread::sleep(2);
}

void SSHTunnel::retrieve_files()
{
    // This tried to call p4 files to get all available files
    // for retrieval, as well as the current client status.

    // first we check if the connection is active. If it's failed,
    // we try to reopen the tunnel.
    if (P.processId() == 0)
    {
        // no active tunnel - try to connect
        run_ssh();

        if (P.processId() == 0)
        {
            // Couldn't get the tunnel to open
            qDebug() << "Tunnel failed to open";
            return;
        }
    }

    // Got a valid SSH process - try to issue the p4 files
    qDebug() << "Trying to P4";

    QProcess files;

    QStringList list;
    list << "-s" << "-p" << "tcp:localhost:1234" << "-u" << PerforceUser << "files" << "//depot/...";

    files.start("/usr/local/bin/p4", list);
    files.waitForFinished();

    QString p4_result = files.readAll();
    QStringList p4_lines = p4_result.split(QRegExp("[\r\n]"),QString::SkipEmptyParts);

    bool do_retry = false;

    for (int i =0; i < p4_lines.count(); i++)
    {
        qDebug() << "p4> " << p4_lines.at(i);
        if (p4_lines.at(i).startsWith("error:"))
        {
            if (p4_lines.at(i).contains("please login again") ||
                p4_lines.at(i).contains("invalid or unset"))
            {
                qDebug() << "Issuing login";

                //
                // issue the login
                //
                QProcess login;
                connect(&login, &QProcess::readyReadStandardError, this, &SSHTunnel::echo_err);
                connect(&login, &QProcess::readyReadStandardOutput, this, &SSHTunnel::echo_std);

                QStringList login_list;
                login_list << "-s" << "-p" << "localhost:1234" << "-u" << PerforceUser << "login";
                login.start("/usr/local/bin/p4", login_list);
                login.waitForStarted();
                login.waitForReadyRead();

                QString login_results = login.readAll();
                if (login_results.startsWith("Enter password:"))
                {
                    
                }
                else
                    qDebug() << login_results;

                login.waitForFinished(5000);
                do_retry = true;
                break;
            }
        }
    }

    if (do_retry == true && retrying == false)
    {
        retrying = true;
        retrieve_files();
        retrying = false;
    }

}

void SSHTunnel::shutdown_tunnel()
{
    // This shutdowns the SSH tunnel if connected.
    qDebug() << "Shutting down SSH...";
    P.terminate();
    P.waitForFinished();
    qDebug() << "...Done";
}

void SSHTunnel::shutdown_fn()
{
    qDebug() << "Shutting Down";
    P.terminate();
    P.waitForFinished();
    qDebug() << "Closed.";

    thread()->quit();
}

void SSHTunnel::echo_err()
{
    qDebug() << "ssh err>" << QString(P.readAllStandardError());
}

void SSHTunnel::echo_std()
{
    qDebug() << "ssh out>" << QString(P.readAllStandardOutput());
}

void MainWindow::TestAction()
{

}

void MainWindow::ShowContextMenu(const QPoint &point)
{
    QMenu contextMenu(tr("Context menu"), this);


    QAction action1("Edit", this);
    connect(&action1, SIGNAL(triggered()), this, SLOT(TestAction()));
    contextMenu.addAction(&action1);


    QAction action2("Subscribe", this);
    connect(&action2, SIGNAL(triggered()), this, SLOT(TestAction()));
    contextMenu.addAction(&action2);

    QAction action3("Unsubscribe", this);
    connect(&action3, SIGNAL(triggered()), this, SLOT(TestAction()));
    contextMenu.addAction(&action3);

    QAction action4("", this);
    connect(&action4, SIGNAL(triggered()), this, SLOT(TestAction()));
    contextMenu.addAction(&action4);



    contextMenu.exec(ui->treeWidget->mapToGlobal(point));
}

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);


    qDebug() << "Creating SSH Tunnel";

    Tunnel = new SSHTunnel();
    Tunnel->moveToThread(&TunnelThread);

    qDebug() << Tunnel->thread();
    qDebug() << Tunnel->P.thread();

    connect(&TunnelThread, &QThread::finished, Tunnel, &QObject::deleteLater);
    //connect(this, &MainWindow::launch, Tunnel, &SSHTunnel::thread_fn);
    TunnelThread.start();


    QVBoxLayout *mainLayout = new QVBoxLayout();
    mainLayout->addWidget(ui->splitter);

    ui->splitter->setChildrenCollapsible(false);
    ui->centralWidget->setLayout(mainLayout);

    ui->treeWidget->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->treeWidget, SIGNAL(customContextMenuRequested(const QPoint &)),
            this, SLOT(ShowContextMenu(const QPoint &)));

    /*
    QHash<QString, QTreeWidgetItem*> tree;
    QFileIconProvider icons;
    QTreeWidgetItem* root = new QTreeWidgetItem((QTreeWidget*)0, QStringList(QString("test")));
    root->setIcon(0, icons.icon(QFileIconProvider::Folder));
    ui->treeWidget->insertTopLevelItem(0, root);

    QDirIterator projects("c:\\devel\\projects\\", QDirIterator::Subdirectories);
    while (projects.hasNext())
    {
        QString file = projects.next();
        file.remove(0, 18);

        QStringList sections = file.split("/");
        if (sections.last() == "." ||
            sections.last() == "..")
            continue;
        // the last entry is the filename
        QString accumulator;
        for (int i = 0; i < sections.size(); i++)
        {
            // find the tree entry for the path.
            QString current;
            if (accumulator.size())
                current = accumulator + "/" + sections[i];
            else
                current = sections[i];

            QHash<QString, QTreeWidgetItem*>::iterator parent_it = tree.find(current);
            QTreeWidgetItem* parent = 0;
            if (parent_it == tree.end())
            {
                // need to create the node.
                QTreeWidgetItem* parent_parent = root;
                if (accumulator.size())
                    parent_parent = tree[accumulator];
                parent = new QTreeWidgetItem(parent_parent, QStringList(sections[i]));
                tree[current] = parent;

            }
            else
                parent = *parent_it;

            accumulator = current;
        }

    }
*/
}

MainWindow::~MainWindow()
{
    delete ui;
}


void MainWindow::closeEvent(QCloseEvent *)
{
    // This has to be here because on OSX we get this twice
    // with a cmd-w
    static bool already_closing = false;
    if (already_closing)
        return;

    already_closing = true;
    qDebug() << "closeEvent";

    // shut down ssh
    QMetaObject::invokeMethod(Tunnel, "shutdown_fn");

    qDebug() << "sent shutdown, killing the thread";

    // Kill the thread and wait
    TunnelThread.wait();

    qDebug() << "done";
}

// This is the SSH user to use for the tunnel.
QString TunnelUser;

// This is the server to connect to with SSH
QString TunnelServer;

// This is the certificate file (.ppk) to use to authenticate the user with SSH.
QString TunnelKeyPathAndFile;

// This is the path and filename of plink.exe
QString PlinkPath;

void MainWindow::on_pushButton_clicked()
{
    QStringList args;
    args << "-noagent" << "-L" << "1234:localhost:1666" << "-i" << TunnelKeyPathAndFile << TunnelUser << "@" << TunnelServer;

    tunnel_process.start(PlinkPath, args);
    tunnel_process.waitForStarted();

    qDebug() << tunnel_process.errorString();

    QByteArray outptu = tunnel_process.readAllStandardOutput();
    qDebug() << outptu;

    ui->textEdit->setText(QString::fromUtf8(outptu));
    qDebug() << "ok";
}
