#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "qfileiconprovider.h"
#include "qprocess.h"
#include "qdebug.h"
#include "qdiriterator.h"
#include "qhash.h"
#include "qmenu.h"

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
