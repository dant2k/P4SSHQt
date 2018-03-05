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
#include "qmessagebox.h"

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

// This is the clientspec for this machine.
QString PerforceClientspec;

// This is the path we set as the root in perforce.
QString PerforceRoot;

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
    list << "-p" << "tcp:localhost:1234" << "-u" << PerforceUser << "print" << "-o" << output_file << ("//depot/" + depot_file);

    files.start(PerforcePath, list);
    files.waitForFinished();
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
bool SSHTunnel::run_perforce_command(QStringList command_and_args, QStringList& output_lines)
{
    // Verify tunnel is open
    if (P.processId() == 0)
    {
        // no active tunnel - try to connect
        run_ssh();

        if (P.processId() == 0)
        {
            // Couldn't get the tunnel to open
            qDebug() << "Tunnel failed to open";
            return false;
        }
    }

    qDebug() << "p4" << command_and_args;

    QStringList list;
    list << "-p" << "tcp:localhost:1234" << "-u" << PerforceUser << "-c" << PerforceClientspec << command_and_args;

    QProcess files;
    files.start(PerforcePath, list);
    files.waitForFinished();

    QString p4_result = files.readAllStandardOutput();

    output_lines = p4_result.split(QRegExp("[\r\n]"),QString::SkipEmptyParts);

    if (files.exitCode() == 0)
        return true;

    QString error = files.readAllStandardError();
    if (error.contains("please login again") ||
        error.contains("invalid or unset"))
    {
        qDebug() << "Login expired.";

        //
        // issue the login
        //
        QProcess login;
        QStringList login_list;
        login_list << "-p" << "localhost:1234" << "-u" << PerforceUser << "login";
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
            bool ret = run_perforce_command(command_and_args, output_lines);
            retrying = false;
            return ret;
        }
        return false;
    } // end if login failure.

    qDebug() << "UNHANDLED ERROR";
    qDebug() << command_and_args;
    qDebug() << output_lines;
    return false;
}

void SSHTunnel::retrieve_files()
{
    QMap<QString, FileEntry>* file_map = new QMap<QString, FileEntry>;

    QStringList server_files;
    QStringList args;
    args << "fstat" << "-T" << "depotFile,clientFile,isMapped,headRev,haveRev,action,otherOpen" << "//depot/...";
    if (run_perforce_command(args, server_files) == false)
        return;

    QString current_depot_file;
    for (int i = 0; i < server_files.length(); i++)
    {
        if (server_files[i].startsWith("... depotFile"))
        {
            current_depot_file = server_files[i].mid(14 + 8);
            file_map->operator [](current_depot_file).depot_file = current_depot_file;
        }
        else if (server_files[i].startsWith("... clientFile"))
            file_map->operator [](current_depot_file).local_file = server_files[i].mid(15);
        else if (server_files[i].startsWith("... isMapped"))
            file_map->operator [](current_depot_file).subscribed = true;
        else if (server_files[i].startsWith("... headRev"))
            file_map->operator [](current_depot_file).head_revision = server_files[i].mid(12).toInt();
        else if (server_files[i].startsWith("... haveRev"))
            file_map->operator [](current_depot_file).local_revision = server_files[i].mid(12).toInt();
        else if (server_files[i].startsWith("... action"))
        {
            // we have it open or for add.
            if (server_files[i].mid(11) == "edit")
                file_map->operator [](current_depot_file).open_for_edit = true;
            else if (server_files[i].mid(11) == "add")
                file_map->operator [](current_depot_file).open_for_add = true;
        }
        else if (server_files[i].startsWith("... otherOpen"))
        {
            // someone else has it open.
            file_map->operator [](current_depot_file).open_by_another = true;
        }
    }

    //
    // Check all files in the enlistment directory to find any files
    // that need to be added.
    //
    QDirIterator it(PerforceRoot, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        QString client_path = it.next();
        if (it.fileInfo().isDir())
            continue;

        // strip off the depot path
        QString depot_path = client_path.mid(PerforceRoot.length() + 1);

        // check if we need to add.
        if (file_map->contains(depot_path) == false)
        {
            // New file - add to the database with no subscription or head rev.
            file_map->operator [](depot_path).local_file = client_path;
        }
    }

    // send this to the forground thread
    QMetaObject::invokeMethod(thunk, "RefreshUIThunk", Q_ARG(void*, file_map));
}

void SSHTunnel::CheckoutFile(QString depot_file)
{
    QStringList server_files;
    QStringList args;
    args << "edit" << "//depot/" + depot_file;
    if (run_perforce_command(args, server_files) == false)
        return;

    retrieve_files();
}

void SSHTunnel::SubmitFiles(void* depot_file_list, QString commit_message)
{
    QStringList* depot_files = (QStringList*)depot_file_list;

    QStringList server_files;
    QStringList args;
    args << "submit";
    args << "-d" << commit_message;
    for (int i = 0; i < depot_files->length(); i++)
        args << "//depot/" + depot_files->at(i);

    if (run_perforce_command(args, server_files) == false)
        return;

    retrieve_files();
}

void SSHTunnel::RevertFiles(void* depot_file_list)
{
    QStringList* depot_files = (QStringList*)depot_file_list;

    QStringList server_files;
    QStringList args;
    args << "revert";
    for (int i = 0; i < depot_files->length(); i++)
        args << "//depot/" + depot_files->at(i);

    if (run_perforce_command(args, server_files) == false)
        return;

    retrieve_files();
}

void SSHTunnel::RevertFile(QString depot_file)
{
    QStringList server_files;
    QStringList args;
    args << "revert" << "//depot/" + depot_file;
    if (run_perforce_command(args, server_files) == false)
        return;

    retrieve_files();
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

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::Context_Edit()
{
    QMetaObject::invokeMethod(Tunnel, "CheckoutFile", Q_ARG(QString, Action_Edit->data().toString()));
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::Context_Revert()
{
    QMetaObject::invokeMethod(Tunnel, "RevertFile", Q_ARG(QString, Action_Revert->data().toString()));
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::TestAction()
{

}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::ShowContextMenu(const QPoint &point)
{
    if (ui->treeWidget->selectedItems().length() == 0)
        return; // no menu if nothing selected

    QString const& selected_depot_file = ui->treeWidget->selectedItems().at(0)->data(0, Qt::UserRole).toString();

    qDebug() << selected_depot_file;

    if (selected_depot_file.length() == 0)
        return; // directory.

    QMenu contextMenu(tr("Context menu"), this);

    FileEntry const& entry = FileMap->operator [](selected_depot_file);

    if (entry.subscribed)
    {
        if (entry.head_revision == entry.local_revision)
        {
            if (entry.open_by_another == false) // can't do anything if locked.
            {
                // open for add shouldn't be in the list on the left.
                Q_ASSERT(entry.open_for_add == false);

                if (entry.open_for_edit == false)
                {
                    Action_Edit->setData(selected_depot_file);
                    contextMenu.addAction(Action_Edit);
                }
                else
                {
                    Action_Revert->setData(selected_depot_file);
                    contextMenu.addAction(Action_Revert);
                }
            }
        }
        else
        {
            // don't have latest - offer sync.
        }

        QAction action3("Unsubscribe", this);
        connect(&action3, SIGNAL(triggered()), this, SLOT(TestAction()));
        contextMenu.addAction(&action3);
    }
    else
    {
        QAction action2("Subscribe", this);
        connect(&action2, SIGNAL(triggered()), this, SLOT(TestAction()));
        contextMenu.addAction(&action2);
    }

    contextMenu.exec(ui->treeWidget->mapToGlobal(point));
}

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    file_list_thunker(this),
    FileMap(0)
{
    ui->setupUi(this);

    Action_Edit = new QAction("Edit", this);
    connect(Action_Edit, SIGNAL(triggered()), this, SLOT(Context_Edit()));
    Action_Revert = new QAction("Revert", this);
    connect(Action_Revert, SIGNAL(triggered()), this, SLOT(Context_Revert()));

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
        PerforceClientspec = settings.value("Perforce/Clientspec", "perforce_client").toString();
        PerforceRoot = settings.value("Perforce/Root", "perforce_root").toString();

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

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void FileListThunk::RefreshUIThunk(void* new_file_list)
{
    QMap<QString, FileEntry>* file_list = (QMap<QString, FileEntry>*)new_file_list;
    thunk_to->RefreshUI(file_list);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::RefreshUI(QMap<QString, FileEntry>* NewFileMap)
{
    //
    // Update the UI tree for the depot.
    // For the moment just recreate entirely.
    //
    ui->treeWidget->clear();
    ui->lstEdited->clear();

    if (FileMap)
        delete FileMap;
    FileMap = 0;
    FileMap = NewFileMap;

    QHash<QString, QTreeWidgetItem*> tree;
    QFileIconProvider icons;
    QTreeWidgetItem* root = new QTreeWidgetItem((QTreeWidget*)0, QStringList(QString("depot")));
    root->setIcon(0, icons.icon(QFileIconProvider::Folder));
    ui->treeWidget->insertTopLevelItem(0, root);

    int out_of_date_count = 0;

    QMap<QString, FileEntry>::const_iterator it = FileMap->constBegin();
    for (; it != FileMap->constEnd(); ++it)
    {
        FileEntry const& entry = it.value();
        QString const& file = it.value().depot_file;

        if (entry.subscribed)
        {
            if (entry.local_revision != entry.head_revision)
            {
                // This file is out of date.
                out_of_date_count++;
            }

            if (entry.open_for_add)
            {
                ui->lstEdited->addItem("(add) " + entry.depot_file);
            }
            if (entry.open_for_edit)
                ui->lstEdited->addItem("(edit) " + entry.depot_file);
        }


        // we can have non-depot files it things need to be added.
        if (file.length() == 0)
            continue;

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

    if (out_of_date_count == 0)
        ui->lblOutOfDate->setText("All files updated.");
    else if (out_of_date_count == 1)
        ui->lblOutOfDate->setText("1 file out of date.");
    else
        ui->lblOutOfDate->setText(out_of_date_count + " files out of date.");

}

MainWindow::~MainWindow()
{
    delete ui;
    delete FileMap;
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

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::on_btnConnect_clicked()
{
    QMetaObject::invokeMethod(Tunnel, "retrieve_files");
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::on_btnRevertSelected_clicked()
{
    QList<QListWidgetItem *> items = ui->lstEdited->selectedItems();
    if (items.length() == 0)
        return;

    QMessageBox msgBox;
    msgBox.setText("This will erase changes!");
    msgBox.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
    msgBox.setDefaultButton(QMessageBox::Cancel);
    int ret = msgBox.exec();
    if (ret != QMessageBox::Ok)
        return;

    QStringList* depot_files = new QStringList();
    for (int i = 0; i < items.length(); i++)
    {
        QString file_text = items.at(i)->text();
        file_text = file_text.mid(file_text.indexOf(' ') + 1);
        depot_files->push_back(file_text);
    }

    QMetaObject::invokeMethod(Tunnel, "RevertFiles", Q_ARG(void*, depot_files));
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::on_btnCommitSelected_clicked()
{
    QList<QListWidgetItem *> items = ui->lstEdited->selectedItems();
    if (items.length() == 0)
        return;

    bool ok = false;
    QString commit_message = QInputDialog::getText(this, "Commit Message", "Description:", QLineEdit::Normal, "", &ok);
    if (ok == false)
        return;

    if (commit_message.length() == 0)
    {
        QMessageBox msgBox;
        msgBox.setText("Requires description.");
        msgBox.setStandardButtons(QMessageBox::Ok);
        msgBox.exec();
        return;
    }

    QStringList* depot_files = new QStringList();
    for (int i = 0; i < items.length(); i++)
    {
        QString file_text = items.at(i)->text();
        file_text = file_text.mid(file_text.indexOf(' ') + 1);
        depot_files->push_back(file_text);
    }

    QMetaObject::invokeMethod(Tunnel, "SubmitFiles", Q_ARG(void*, depot_files), Q_ARG(QString, commit_message));
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
