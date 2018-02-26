#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "qfileiconprovider.h"
#include "qprocess.h"
#include "qdebug.h"
#include "qdiriterator.h"
#include "qhash.h"
#include "qmenu.h"
#include "qsettings.h"
#include "qinputdialog.h"
#include "qfiledialog.h"


// This is the SSH user to use for the tunnel.
QString TunnelUser;

// This is the server to connect to with SSH
QString TunnelServer;

// This is the certificate file (.ppk) to use to authenticate the user with SSH.
QString TunnelKeyPathAndFile;

// This is the path and filename of plink.exe (or ssh)
QString PlinkPath;

// This is the path and filename of p4.exe
QString PerforcePath;

// This is the perforce username (NOT the tunnel username!!)
QString PerforceUser;

// This is the password for _perforce_, not the tunnel key.
// Not stored in
QString PerforcePassword;

SSHTunnel::SSHTunnel(FileListThunk* _thunk) : P(this), thunk(_thunk)
{
    retrying = false;

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
    list << "-N" << "-L" << "1234:localhost:1666" << "-i" << TunnelKeyPathAndFile << (TunnelUser + "@" + TunnelServer);

    qDebug() << "Running SSH";

    P.start(PlinkPath, list);
    P.waitForStarted();

    // The tunnel is connecting in the background, however there's no signal
    // for us to know once it's done connecting, so we just sleep 5 seconds.
    qDebug() << "Waiting for connection...";
    QThread::sleep(2);
}

void SSHTunnel::download_file_to(QString depot_file, QString dest_dir)
{
    if (P.processId() == 0)
        return;

    QString output_file = dest_dir + depot_file.mid(depot_file.lastIndexOf('/'));

    QProcess files;

    QStringList list;
    list << "-s" << "-p" << "tcp:localhost:1234" << "-u" << PerforceUser << "print" << "-o" << output_file << ("//depot/" + depot_file);

    files.start(PerforcePath, list);
    files.waitForFinished();
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

    files.start(PerforcePath, list);
    files.waitForFinished();

    QString p4_result = files.readAll();
    QStringList p4_lines = p4_result.split(QRegExp("[\r\n]"),QString::SkipEmptyParts);

    if (files.exitCode() == 0)
    {
        QVector<QString>* file_list = new QVector<QString>();

        // got the files - parse to get the list of files available.
        for (int i = 0; i < p4_lines.length(); i++)
        {
            if (p4_lines[i].startsWith("info:") == false)
                continue;
            QString file = p4_lines[i].mid(6 + 8); // info: //depot/
            file = file.left(file.indexOf('#'));
            file_list->push_back(file);
        }

        // send this to the forground thread
        QMetaObject::invokeMethod(thunk, "update_file_list", Q_ARG(void*, file_list));
    }
    else
    {
        // Did we fail due to an expired login?
        if (p4_lines.at(0).startsWith("error:"))
        {
            if (p4_lines.at(0).contains("please login again") ||
                p4_lines.at(0).contains("invalid or unset"))
            {
                qDebug() << "Issuing login";

                //
                // issue the login
                //
                QProcess login;
                QStringList login_list;
                login_list << "-s" << "-p" << "localhost:1234" << "-u" << PerforceUser << "login";
                login.start(PerforcePath, login_list);
                login.waitForStarted();
                login.waitForReadyRead();

                QString login_results = login.readAll();
                qDebug() << login_results;

                if (login_results.startsWith("Enter password:"))
                {
                    login.write(PerforcePassword.toUtf8(), PerforcePassword.length());
                    login.write("\n", 1);
                }
                else
                    qDebug() << login_results;


                login.waitForFinished(5000);

                bool do_retry = false;

                qDebug() << "Retrying" << do_retry << retrying;

                if (do_retry == true && retrying == false)
                {
                    retrying = true;
                    retrieve_files();
                    retrying = false;
                }
            } // end if login failure.
        } // end if error:
    } // end if p4 files failed.
}

void SSHTunnel::shutdown_tunnel()
{
    // This shutdowns the SSH tunnel if connected.
    qDebug() << "Shutting down SSH...";
    P.kill();
    P.waitForFinished();
    qDebug() << "...Done";
}

void SSHTunnel::shutdown_fn()
{
    qDebug() << "Shutting Down";
    P.kill();
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
    ui(new Ui::MainWindow),
    file_list_thunker(ui)
{
    ui->setupUi(this);

    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "Midnight Oil Games", "P4SSHQt");

    if (false)
    {
        // just for setting up the settings...
        settings.setValue("Tunnel/User", "tunnel_user");
        settings.setValue("Tunnel/Server", "tunnel_server");
        settings.setValue("Tunnel/Key", "tunnel_key");
        settings.setValue("Tunnel/SSH", "tunnel_ssh");
        settings.setValue("Perforce/User", "perforce_user");
        settings.setValue("Perforce/Path", "perforce_path");
    }
    else
    {
        TunnelUser = settings.value("Tunnel/User", "tunnel_user").toString();
        TunnelServer = settings.value("Tunnel/Server", "tunnel_server").toString();
        TunnelKeyPathAndFile = settings.value("Tunnel/Key", "tunnel_key").toString();
        PlinkPath = settings.value("Tunnel/SSH", "tunnel_ssh").toString();
        PerforceUser = settings.value("Perforce/User", "perforce_user").toString();
        PerforcePath = settings.value("Perforce/Path", "perforce_path").toString();

        // Normally this will be done via dialog, however for the moment..
        PerforcePassword = settings.value("Perforce/Pass", "perforce_pass").toString();
    }

    //PerforcePassword = QInputDialog::getText(this, "Perforce Password", "Password:", QLineEdit::Password);

    qDebug() << "Creating SSH Tunnel";

    Tunnel = new SSHTunnel(&file_list_thunker);
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

}

void FileListThunk::update_file_list(void* new_file_list)
{
    QVector<QString>* file_list = (QVector<QString>*)new_file_list;
    qDebug() << "got it";
    qDebug() << *file_list;

    qDebug() << main_window_ptr;
    qDebug() << main_window_ptr->treeWidget;

    QHash<QString, QTreeWidgetItem*> tree;
    QFileIconProvider icons;
    QTreeWidgetItem* root = new QTreeWidgetItem((QTreeWidget*)0, QStringList(QString("test")));
    root->setIcon(0, icons.icon(QFileIconProvider::Folder));
    main_window_ptr->treeWidget->insertTopLevelItem(0, root);

    for (int i = 0; i < file_list->length(); i++)
    {
        QString file = file_list->at(i);

        QStringList sections = file.split("/");
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
                if (i == sections.length()-1)
                {
                    // this is the file level - set the file path in the data
                    parent->setData(0, Qt::UserRole, file);
                }
                tree[current] = parent;

            }
            else
                parent = *parent_it;

            accumulator = current;
        }

    }


    delete file_list;
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


void MainWindow::on_pushButton_clicked()
{
    QMetaObject::invokeMethod(Tunnel, "retrieve_files");
}

void MainWindow::on_treeWidget_itemDoubleClicked(QTreeWidgetItem *item, int column)
{
    QString file_name = item->data(0, Qt::UserRole).toString();

    if (file_name.length() == 0)
        return; // not an actual file.

    // got a file, ask where to put it.
    QString save_to = QFileDialog::getExistingDirectory(this, "Save To..");

    // send to the background thread to process.
    QMetaObject::invokeMethod(Tunnel, "download_file_to", Q_ARG(QString, file_name), Q_ARG(QString, save_to));
}
