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
#include "qplaintextedit.h"
#include "qdialogbuttonbox.h"
#include "qdesktopservices.h"

//working
/*
 * -- need partial queue process on failure
 * -- not sure if I should even have queues to be honest. Seems pointless? Just make sure things
 *      run in the background.
 * -- need to delete the changelist on a failed submission.
 * -- need commands on an entire folder.
 * -- add a "reconcile changes?"
 * -- write a wrapper program for plink and ssh that just monitors for the parent process dying
 *      and kills the tunnel to try and avoid shitlets.
 * -- check for the port being open on connection instead of waiting forever - the pause on
 *      open actually sucks.
 *
 * ++ Need to handle incorrect password better... currently kinda silently fails.
 *      Now asks again. Doesn't requeue the last operation, but fine for now.
 * ++ need to get the listbox columns sizing so we can see the entire filename, or just
 *      put everything in one column.
 *      Fixed - last two columns are fixed pixel width!
 * ++ need version history.
 *      Version history added! can't do details yet tho...
 * ++ need to test adding a new folder
 *      this works on windows, also tested spaces in filenames.
 * ++ adding file descriptions.
 *      adding a file now you can add the changelist description.
 * ++ need delete from disc for files that are new.
 *      Now works
 * ++ need to open folder for a file.
 *      This works on windows, just need to do the bit for OSX once I'm on my laptop.
 * ++ try to find a way to open associated program for an edited file?
 *      Now can double click on a file to open the associated program - if the file is
 *      subscribed.
 * ++ need to be able to edit the queue after I add something in case its wrong.
 *      can now delete items in the queue.
 * */

// This is the SSH user to use for the tunnel.
static QString TunnelUser;

// This is the server to connect to with SSH
static QString TunnelServer;

// This is the certificate file (.ppk) to use to authenticate the user with SSH.
static QString TunnelKeyPathAndFile;

// This is the path and filename of plink.exe (or ssh)
static QString PlinkPath;

// This is the path and filename of p4.exe
static QString PerforcePath;

// This is the perforce username (NOT the tunnel username!!)
static QString PerforceUser;

// This is the password for _perforce_, not the tunnel key.
// Not stored in
static QString PerforcePassword;

// This is the clientspec for this machine.
static QString PerforceClientspec;

// This is the path we set as the root in perforce.
static QString PerforceRoot;

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
SSHTunnel::SSHTunnel(FileListThunk* _thunk) : P(this), thunk(_thunk)
{
    retrying = false;

    connect(&P, &QProcess::readyReadStandardError, this, &SSHTunnel::echo_err);
    connect(&P, &QProcess::readyReadStandardOutput, this, &SSHTunnel::echo_std);
    connect(&P, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &SSHTunnel::tunnel_closed);
}
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SSHTunnel::tunnel_closed(int, QProcess::ExitStatus)
{
    qDebug() << "SSL Tunnel Closed";
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
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

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
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
void SSHTunnel::get_file_history(QString depot_file)
{
    if (P.processId() == 0)
        return;

    QStringList args;
    args << "filelog" << ("//depot/" + depot_file);

    QStringList* output = new QStringList();
    if (run_perforce_command(args, QString(), *output))
    {
        QMetaObject::invokeMethod(thunk, "PostFileHistoryThunk", Q_ARG(QString, depot_file), Q_ARG(void*, output));
    }
    else
        delete output;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
bool SSHTunnel::run_perforce_command(QStringList command_and_args, QString const& std_input, QStringList& output_lines)
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
    if (std_input.length())
    {
        files.waitForStarted();
        files.write(std_input.toLatin1());
        files.closeWriteChannel();
    }
    files.waitForFinished();

    QString p4_result = files.readAllStandardOutput();

    output_lines = p4_result.split(QRegExp("[\r\n]"),QString::SkipEmptyParts);

    if (files.exitCode() == 0)
        return true;

    QString error = files.readAllStandardError();
    if (error.contains("please login again") ||
        error.contains("invalid or unset"))
    {
        if (PerforcePassword.length() == 0)
            return false; // no password to try, just bail. It means they cancelled out.

        if (retrying == true)
        {
            // Password incorrect - post a msg to the foreground.
            QMetaObject::invokeMethod(thunk, "BadPasswordThunk");
            return false;
        }
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

        // Now that we've logged in - try to run the command we were issued.
        bool do_retry = true;

        qDebug() << "Retrying" << do_retry << retrying;

        if (do_retry == true && retrying == false)
        {
            retrying = true;
            bool ret = run_perforce_command(command_and_args, QString(),output_lines);
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

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
QMap<QString, FileEntry>* SSHTunnel::retrieve_files(bool post_to_foreground)
{
    QMap<QString, FileEntry>* file_map = new QMap<QString, FileEntry>;

    QStringList* server_changes = new QStringList();
    {
        QStringList args;
        args << "changes" << "-s" << "submitted" << "-m" << "100";
        if (run_perforce_command(args, QString(), *server_changes) == false)
            return nullptr;
    }

    QStringList server_files;
    {
        QStringList args;
        args << "fstat" << "-F" << "^headAction=delete" << "-T" << "depotFile,clientFile,headRev,haveRev,action,otherOpen0" << "//depot/...";
        if (run_perforce_command(args, QString(), server_files) == false)
            return nullptr;
    }

    QStringList revert_list;
    QStringList remove_from_files_list;

    QSet<QString> client_folders;

    QString current_depot_file;
    for (int i = 0; i < server_files.length(); i++)
    {
        if (server_files[i].startsWith("... depotFile"))
        {
            current_depot_file = server_files[i].mid(14 + 8);
            file_map->operator [](current_depot_file).depot_file = current_depot_file;
            file_map->operator [](current_depot_file).exists_in_depot = true;
        }
        else if (server_files[i].startsWith("... clientFile"))
        {
            file_map->operator [](current_depot_file).local_file = server_files[i].mid(15);

            // track the different folders so we can make sure they are created.
            QString& local_file = file_map->operator [](current_depot_file).local_file;
            client_folders.insert(local_file.left(local_file.lastIndexOf(QDir::separator())));
        }
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
            {
                // if an add failed, then the file is on disc but not in the server
                // list - which means it'll get added below in the consistency check.
                revert_list << current_depot_file;
                remove_from_files_list << current_depot_file;
            }
            else if (server_files[i].mid(11) == "delete")
            {
                // if a delete failed, then the file is on the server and on disc -
                // so its just a normal unedited file at this point.
                revert_list << current_depot_file;
            }
        }
        else if (server_files[i].startsWith("... ... otherOpen0"))
        {
            // someone else has it open.
            file_map->operator [](current_depot_file).open_by_another = server_files[i].mid(19);
        }
    }

    //
    // Make sure the local directories exist, so that it's easy to add files.
    //
    foreach (const QString& d, client_folders)
    {
        QDir dir(d);
        if (dir.exists() == false)
        {
            dir.mkpath(".");
        }
    }

    //
    // Any files that are open for add or delete represent failures of previous operations -
    // revert those actions
    //
    if (revert_list.length())
    {
        QStringList args;
        args << "revert" << "-k" << revert_list;
        revert_list.clear();
        run_perforce_command(args, QString(), revert_list);
    }

    // these files need to be removed from server_files.
    for (int i = 0; i < remove_from_files_list.length(); i++)
    {
        file_map->remove(remove_from_files_list[i]);
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
            file_map->operator [](depot_path).depot_file = depot_path;
            file_map->operator [](depot_path).exists_in_depot = false;
        }
    }

    if (post_to_foreground)
    {
        // send this to the forground thread
        QMetaObject::invokeMethod(thunk, "RefreshUIThunk", Q_ARG(void*, file_map), Q_ARG(void*, server_changes));
    }
    else
        delete server_changes;
    return file_map;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SSHTunnel::shutdown_tunnel()
{
    // This shutdowns the SSH tunnel if connected.
    qDebug() << "Shutting down SSH...";
    P.kill();
    P.waitForFinished();
    qDebug() << "...Done";
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SSHTunnel::shutdown_fn()
{
    qDebug() << "Shutting Down";
    P.kill();
    P.waitForFinished();
    qDebug() << "Closed.";

    thread()->quit();
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SSHTunnel::echo_err()
{
    qDebug() << "ssh err>" << QString(P.readAllStandardError());
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SSHTunnel::echo_std()
{
    qDebug() << "ssh out>" << QString(P.readAllStandardOutput());
}

enum QueuedActionType
{
    QA_Edit,
    QA_Revert,
    QA_Commit,
    QA_Add,
    QA_Subscribe,
    QA_Unsubscribe,
    QA_Sync,
    QA_ForceSync,
    QA_GetLatest,
    QA_Download,
    QA_Delete
};

struct QueuedAction
{
    QString depot_file;
    QString relevant_file;
    QueuedActionType action;
    QStringList commit_files;
};

static QVector<QueuedAction*> queued_actions;

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
static bool validate_edit(QMap<QString, FileEntry>& files, QString& file_to_edit)
{
    if (files.contains(file_to_edit) == false)
        return false; // file doesn't exist

    FileEntry& entry = files[file_to_edit];
    if (entry.exists_in_depot == false)
        return false;
    if (entry.open_by_another.length())
        return false;
    if (entry.local_revision != entry.head_revision)
        return false;
    if (entry.open_for_edit)
        return true; // this technically is a non-op, so allow it.
    return true;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
static bool validate_delete(QMap<QString, FileEntry>& files, QString& file_to_edit)
{
    if (files.contains(file_to_edit) == false)
        return false; // file doesn't exist

    FileEntry& entry = files[file_to_edit];
    if (entry.exists_in_depot == false)
        return false;
    if (entry.open_by_another.length())
        return false;
    if (entry.local_revision != entry.head_revision)
        return false;
    if (entry.open_for_edit)
        return false;
    return true;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
static bool validate_revert(QMap<QString, FileEntry>& files, QString& file_to_revert)
{
    if (files.contains(file_to_revert) == false)
        return false; // file doesn't exist

    FileEntry& entry = files[file_to_revert];
    if (entry.open_for_edit == false)
        return false;
    return true;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
static bool validate_add(QMap<QString, FileEntry>& files, QString& file_to_add)
{
    if (files.contains(file_to_add) == false)
        return false; // file doesn't exist

    FileEntry& entry = files[file_to_add];
    if (entry.exists_in_depot)
        return false;
    return true;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
static bool validate_sync(QMap<QString, FileEntry>& files, QString& file_to_add)
{
    if (files.contains(file_to_add) == false)
        return false; // file doesn't exist

    FileEntry& entry = files[file_to_add];
    if (entry.exists_in_depot == false)
        return false;
    if (entry.open_for_edit)
        return false; // must revert first
    if (entry.local_revision == 0)
        return false; // must subscribe first.
    if (entry.local_revision == entry.head_revision)
        return false; // already synced - must force sync
    return true;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
static bool validate_forcesync(QMap<QString, FileEntry>& files, QString& file_to_add)
{
    if (files.contains(file_to_add) == false)
        return false; // file doesn't exist

    FileEntry& entry = files[file_to_add];
    if (entry.exists_in_depot == false)
        return false;
    if (entry.open_for_edit)
        return false; // must revert first
    if (entry.local_revision == 0)
        return false; // must subscribe first.
    return true;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
static bool validate_subscribe(QMap<QString, FileEntry>& files, QString& file_to_add)
{
    if (files.contains(file_to_add) == false)
        return false; // file doesn't exist

    FileEntry& entry = files[file_to_add];
    if (entry.exists_in_depot == false)
        return false;
    if (entry.local_revision != 0)
        return false; // already subbed?
    return true;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
static bool validate_unsubscribe(QMap<QString, FileEntry>& files, QString& file_to_add)
{
    if (files.contains(file_to_add) == false)
        return false; // file doesn't exist

    FileEntry& entry = files[file_to_add];
    if (entry.exists_in_depot == false ||
        entry.open_for_edit)
        return false;
    if (entry.local_revision == 0)
        return false; // already unsubbed?
    return true;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
static bool validate_submit(QMap<QString, FileEntry>& files, QStringList& files_to_submit)
{
    foreach (QString const& file, files_to_submit)
    {
        if (files.contains(file) == false)
            return false;
        FileEntry& entry = files[file];

        // to submit, we have to have it open for edit (and no one else can)
        // and it has to be at the head revision, and actually in the depot.
        if (entry.exists_in_depot == false ||
            entry.open_for_edit == false ||
            entry.local_revision != entry.head_revision ||
            entry.open_by_another.length())
            return false;
    }
    return true;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void SSHTunnel::RunQueue()
{
    // Make sure we have the latest state from the server, it case we
    // were opened from a hibernate/sleep state and files changed from
    // another machine.
    QMap<QString, FileEntry>* current_files = retrieve_files(false);
    if (current_files == nullptr)
    {
        // unable to open tunnel - bail on the queue
        return;
    }
    QMap<QString, FileEntry>& files = *current_files; // for syntax insanity

    for (int i = 0; i < queued_actions.length(); i++)
    {
        QStringList args;
        QStringList server_files;
        switch (queued_actions[i]->action)
        {
        case QA_Edit:
        {
            if (validate_edit(files, queued_actions[i]->depot_file))
            {
                args << "edit" << "//depot/" + queued_actions[i]->depot_file;
                if (run_perforce_command(args, QString(), server_files))
                {
                    files[queued_actions[i]->depot_file].open_for_edit = true;
                }
            }

            break;
        }
        case QA_Sync:
        {
            if (validate_sync(files, queued_actions[i]->depot_file))
            {
                args << "sync" << "//depot/" + queued_actions[i]->depot_file;
                if (run_perforce_command(args, QString(), server_files))
                    files[queued_actions[i]->depot_file].local_revision = files[queued_actions[i]->depot_file].head_revision;
            }
            break;
        }
        case QA_ForceSync:
        {
            if (validate_forcesync(files, queued_actions[i]->depot_file))
            {
                args << "sync" << "-f" << "//depot/" + queued_actions[i]->depot_file;
                if (run_perforce_command(args, QString(), server_files))
                    files[queued_actions[i]->depot_file].local_revision = files[queued_actions[i]->depot_file].head_revision;
            }
            break;
        }
        case QA_Delete:
        {
            if (validate_delete(files, queued_actions[i]->depot_file))
            {
                // create a changelist.
                // add the file to the changelist
                // submit the changelist.
                args << "change" << "-i";
                QString changelist = "Change: new\nDescription: Deleting File\n";
                QStringList results;
                if (run_perforce_command(args, changelist, results))
                {
                    // get the changelist ID.
                    if (results.length())
                    {
                        QString parse = results.at(0);
                        parse = parse.mid(7);
                        parse = parse.left(parse.indexOf(' '));
                        qDebug() << "Using Change: " << parse;

                        args.clear();
                        args << "delete" << "-c" << parse << "//depot/" + queued_actions[i]->depot_file;
                        if (run_perforce_command(args, QString(), server_files))
                        {
                            qDebug() << "Opened for delete";

                            args.clear();
                            args << "submit" << "-c" << parse;
                            if (run_perforce_command(args, QString(), server_files))
                            {
                                qDebug() << "Done";
                                files.remove(queued_actions[i]->depot_file);
                            }
                        }
                    }
                }
            }
            break;
        }
        case QA_GetLatest:
        {
            // We want to sync all the files that are subscribed, out of date, and not edited.
            QString std_input_file_list;
            std_input_file_list += "-L\n";

            int file_count = 0;
            QMap<QString, FileEntry>::const_iterator it = files.constBegin();
            for (; it != files.constEnd(); ++it)
            {
                FileEntry const& entry = it.value();
                QString const& file = it.value().depot_file;

                if (entry.local_revision &&
                    entry.local_revision != entry.head_revision &&
                    entry.open_for_edit == false)
                {
                    std_input_file_list += "//depot/" + file + "#" + QString::number(entry.head_revision) + "\n";
                    file_count++;
                }
            }

            // Don't EVER do a sync without a file, because that amounts to subscribing
            // to the entire depot.
            if (file_count > 0)
            {
                args << "-x" << "-" << "sync";
                if (run_perforce_command(args, std_input_file_list, server_files))
                {
                    QMap<QString, FileEntry>::iterator it = files.begin();
                    for (; it != files.end(); ++it)
                    {
                        FileEntry& entry = it.value();
                        if (entry.local_revision &&
                            entry.local_revision != entry.head_revision &&
                            entry.open_for_edit == false)
                        {
                            entry.local_revision = entry.head_revision;
                        }
                    }
                }
            }

            break;
        }
        case QA_Subscribe:
        {
            // A subscribed file is one we have synced at all (ie not at 0/N).
            // So subscribing is just syncing the file to head.
            if (validate_subscribe(files, queued_actions[i]->depot_file))
            {
                args << "sync" << "//depot/" + queued_actions[i]->depot_file;
                if (run_perforce_command(args, QString(), server_files))
                    files[queued_actions[i]->depot_file].local_revision = files[queued_actions[i]->depot_file].head_revision;
            }
            break;
        }
        case QA_Unsubscribe:
        {
            // Since a subscribed file is synced to not 0/N, unsubscribing is syncing the
            // file to 0.
            if (validate_unsubscribe(files, queued_actions[i]->depot_file))
            {
                args << "sync" << "//depot/" + queued_actions[i]->depot_file + "#0";
                if (run_perforce_command(args, QString(), server_files))
                    files[queued_actions[i]->depot_file].local_revision = 0;
            }
            break;
        }
        case QA_Download:
        {
            args << "print" << "-o" << queued_actions[i]->relevant_file << "//depot/" + queued_actions[i]->depot_file;
            run_perforce_command(args, QString(), server_files);
            qDebug() << server_files;
            break;
        }
        case QA_Commit:
        {
            if (validate_submit(files, queued_actions[i]->commit_files))
            {
                // the changelist desc needs a space before each line.
                QStringList desc_each_line = queued_actions[i]->relevant_file.split('\n');
                QString change_desc = "Change: new\nDescription: ";
                for (int i = 0; i < desc_each_line.size(); i++)
                {
                    change_desc += desc_each_line[i] + "\n";
                    if (i < desc_each_line.size() - 1)
                        change_desc += "\t";
                }
                change_desc += "Files:";

                foreach (QString const& str, queued_actions[i]->commit_files)
                {
                    change_desc += "\t//depot/" + str + "\n";
                }

                args << "change" << "-i";
                QStringList results;
                if (run_perforce_command(args, change_desc, results))
                {
                    // get the changelist ID.
                    if (results.length())
                    {
                        QString parse = results.at(0);
                        parse = parse.mid(7);
                        parse = parse.left(parse.indexOf(' '));
                        qDebug() << "Using Change: " << parse;

                        args.clear();
                        args << "submit" << "-c" << parse;
                        if (run_perforce_command(args, QString(), server_files))
                        {
                            qDebug() << "Done";
                            // submit succeeded - so we got a new revision, which we have,
                            // and it isn't edited anymore.
                            foreach (QString const& str, queued_actions[i]->commit_files)
                            {
                                files[str].head_revision++;
                                files[str].local_revision++;
                                files[str].open_for_edit = false;
                            }
                        }
                        else
                        {
                            // If the submit failed, then all of our files are now in
                            // a random changelist, when we want them in the default
                            // changelist.

                            QString std_input_file_list = "-c\n";
                            std_input_file_list += "default\n";

                            foreach (QString const& str, queued_actions[i]->commit_files)
                            {
                                std_input_file_list += "//depot/" + str + "\n";
                            }

                            args.clear();
                            args << "-x" << "-" << "reopen";
                            run_perforce_command(args, std_input_file_list, server_files);

                            // \todo delete the empty changelist... get it from teh output
                            // from the fail?
                        }
                    }
                }
            }
            break;
        }
        case QA_Revert:
        {
            if (validate_revert(files, queued_actions[i]->depot_file))
            {
                args << "revert" << "//depot/" + queued_actions[i]->depot_file;

                if (run_perforce_command(args, QString(), server_files))
                {
                    files[queued_actions[i]->depot_file].open_for_edit = false;
                }
            }
            break;
        }
        case QA_Add:
        {
            if (validate_add(files, queued_actions[i]->depot_file))
            {
                // create a changelist.
                // add the file to the changelist
                // submit the changelist.
                args << "change" << "-i";
                QString changelist = "Change: new\nDescription: " + queued_actions[i]->relevant_file + "\n";
                QStringList results;
                if (run_perforce_command(args, changelist, results))
                {
                    // get the changelist ID.
                    if (results.length())
                    {
                        QString parse = results.at(0);
                        parse = parse.mid(7);
                        parse = parse.left(parse.indexOf(' '));
                        qDebug() << "Using Change: " << parse;

                        args.clear();
                        args << "add" << "-c" << parse << "-t" << "+l" << "//depot/" + queued_actions[i]->depot_file;
                        if (run_perforce_command(args, QString(), server_files))
                        {
                            qDebug() << "Opened for add";

                            args.clear();
                            args << "submit" << "-c" << parse;
                            if (run_perforce_command(args, QString(), server_files))
                            {
                                qDebug() << "Done";
                                files[queued_actions[i]->depot_file].exists_in_depot = true;
                                files[queued_actions[i]->depot_file].local_revision = 1;
                                files[queued_actions[i]->depot_file].head_revision = 1;
                            }
                        }
                    }
                }

            }
            break;
        }
        } // end switch action

        delete queued_actions[i];
    } // end for each action
    queued_actions.clear();

    QStringList* server_changes = new QStringList();
    {
        QStringList args;
        args << "changes" << "-s" << "submitted" << "-m" << "100";
        run_perforce_command(args, QString(), *server_changes);
    }

    // Send the updated files list to the UI.
    QMetaObject::invokeMethod(thunk, "RefreshUIThunk", Q_ARG(void*, current_files), Q_ARG(void*, server_changes));
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::Context_Edit()
{
    QueuedAction* qa = new QueuedAction();
    qa->action = QA_Edit;
    qa->depot_file = Action_Edit->data().toString();

    QListWidgetItem* item = new QListWidgetItem("<edit> " + qa->depot_file);
    ui->lstQueue->addItem(item);

    queued_actions.push_back(qa);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::Context_Revert()
{
    QMessageBox msgBox;
    msgBox.setText("This may erase changes!");
    msgBox.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
    msgBox.setDefaultButton(QMessageBox::Cancel);
    int ret = msgBox.exec();
    if (ret != QMessageBox::Ok)
        return;

    QueuedAction* qa = new QueuedAction();
    qa->action = QA_Revert;
    qa->depot_file = Action_Revert->data().toString();

    QListWidgetItem* item = new QListWidgetItem("<revert> " + qa->depot_file);
    ui->lstQueue->addItem(item);

    queued_actions.push_back(qa);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::Context_AddToDepot()
{
    bool ok = false;
    QString add_desc = QInputDialog::getText(this, "File Description", "Description:", QLineEdit::Normal, "Adding File.", &ok);
    if (ok == false)
        return;

    if (add_desc.length() == 0)
        add_desc = "Adding File.";

    QueuedAction* qa = new QueuedAction();
    qa->action = QA_Add;
    qa->depot_file = Action_AddToDepot->data().toString();
    qa->relevant_file = add_desc;

    QListWidgetItem* item = new QListWidgetItem("<add> " + qa->depot_file);
    ui->lstQueue->addItem(item);

    queued_actions.push_back(qa);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::Context_DeleteDepot()
{
    QueuedAction* qa = new QueuedAction();
    qa->action = QA_Delete;
    qa->depot_file = Action_DeleteDepot->data().toString();

    QListWidgetItem* item = new QListWidgetItem("<delete> " + qa->depot_file);
    ui->lstQueue->addItem(item);

    queued_actions.push_back(qa);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::Context_DeleteDisc()
{
    QMessageBox msgBox;
    msgBox.setText("Permanently Delete?");
    msgBox.setInformativeText("This file will be *permanently* deleted, as it has NOT been added to Perforce.\n\n" + Action_DeleteDisc->data().toString());
    msgBox.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
    msgBox.setDefaultButton(QMessageBox::Cancel);
    int ret = msgBox.exec();

    if (ret != QMessageBox::Ok)
        return;

    QString file_string =  PerforceRoot + QDir::separator() + Action_DeleteDisc->data().toString();
    qDebug() << file_string;
    QFile file_on_disc(file_string);
    if (file_on_disc.remove() == false)
    {
        QMessageBox errBox;
        errBox.setText("Failed");
        errBox.setInformativeText("Failed to delete! Check and make sure the file isn't open in any application.");
        errBox.setStandardButtons(QMessageBox::Ok);
        errBox.exec();
        return;
    }

    // Need to update the UI -- so we'll hijack the refresh from the other thread by
    // duplicating the map on the foreground thread and pretending we did something
    // on the background thread.

    // This is apparently how you deep copy a map in QT.
    QMap<QString, FileEntry>* file_map = new QMap<QString, FileEntry>;
    *file_map = *FileMap;
    file_map->detach();

    QStringList* changes = new QStringList();
    *changes = *ServerChanges;

    // Remove the file we just deleted.
    file_map->remove(Action_DeleteDisc->data().toString());
    RefreshUI(file_map, changes);

}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::Context_Subscribe()
{
    QueuedAction* qa = new QueuedAction();
    qa->action = QA_Subscribe;
    qa->depot_file = Action_Subscribe->data().toString();

    QListWidgetItem* item = new QListWidgetItem("<subscribe> " + qa->depot_file);
    ui->lstQueue->addItem(item);

    queued_actions.push_back(qa);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::Context_Unsubscribe()
{
    QueuedAction* qa = new QueuedAction();
    qa->action = QA_Unsubscribe;
    qa->depot_file = Action_Unsubscribe->data().toString();

    QListWidgetItem* item = new QListWidgetItem("<unsubscribe> " + qa->depot_file);
    ui->lstQueue->addItem(item);

    queued_actions.push_back(qa);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::Context_Download()
{
    // Get the destination file.

    QString depot_file = Action_Download->data().toString();
    QString file_name_only = depot_file.mid(depot_file.lastIndexOf('/') + 1);

    QString save_to = QFileDialog::getExistingDirectory(this, "Save " + file_name_only + "To...");


    QueuedAction* qa = new QueuedAction();
    qa->action = QA_Download;
    qa->depot_file = depot_file;
    qa->relevant_file = save_to + "/" + file_name_only;

    QListWidgetItem* item = new QListWidgetItem("<download> " + qa->depot_file + " to " + qa->relevant_file);
    ui->lstQueue->addItem(item);

    queued_actions.push_back(qa);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::Context_Sync()
{
    QueuedAction* qa = new QueuedAction();
    qa->action = QA_Sync;
    qa->depot_file = Action_Sync->data().toString();

    QListWidgetItem* item = new QListWidgetItem("<sync> " + qa->depot_file);
    ui->lstQueue->addItem(item);

    queued_actions.push_back(qa);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::Context_ShowInExplorer()
{
#ifdef Q_OS_MAC
    QStringList args;
    args << "-e";
    args << "tell application \"Finder\"";
    args << "-e";
    args << "activate";
    args << "-e";
    args << "select POSIX file \"" + QDir::toNativeSeparators(PerforceRoot) + QDir::separator() + QDir::toNativeSeparators(Action_ShowInExplorer->data().toString()) + "\"";
    args << "-e";
    args << "end tell";
    QProcess::startDetached("osascript", args);
#endif
#ifdef Q_OS_WIN32
    QString command = "explorer /select," + QDir::toNativeSeparators(PerforceRoot) + QDir::separator() + QDir::toNativeSeparators(Action_ShowInExplorer->data().toString());
    QProcess::startDetached(command);
#endif

}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::Context_ForceSync()
{
    QueuedAction* qa = new QueuedAction();
    qa->action = QA_ForceSync;
    qa->depot_file = Action_ForceSync->data().toString();

    QListWidgetItem* item = new QListWidgetItem("<force sync> " + qa->depot_file);
    ui->lstQueue->addItem(item);

    queued_actions.push_back(qa);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::Context_RemoveFromQueue()
{
    int index_to_remove = Action_RemoveFromQueue->data().toInt();
    queued_actions.remove(index_to_remove);
    delete ui->lstQueue->item(index_to_remove);
}
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::on_btnRunQueue_clicked()
{
    //
    // We go over every entry in the queue list and send it to the background
    // thread to run.
    //
    QMetaObject::invokeMethod(Tunnel, "RunQueue");
    ui->lstQueue->clear();
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::on_btnOpenP4_clicked()
{
    QProcess p;
//    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
//    env.insert("P4USER", PerforceUser);
//    env.insert("P4CLIENT", PerforceClientspec);
//    env.insert("P4PORT", "localhost:1234");
//    p.setProcessEnvironment(env);
    QStringList args;
    args << "-q" << "-u" << PerforceUser << "-p" << "localhost:1234" << "-c" << PerforceClientspec;
    p.startDetached("c:/Perforce/P4win.exe", args);
}


//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::ShowEditedContextMenu(const QPoint& point)
{
    if (ui->lstEdited->selectedItems().length() == 0)
        return;

    QPoint global_point = ui->lstEdited->mapToGlobal(point);

    QMenu contextMenu(tr("Context menu"), this);

    Action_Revert->setData(ui->lstEdited->selectedItems()[0]->text());
    contextMenu.addAction(Action_Revert);
    contextMenu.exec(global_point);

}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::ShowQueueContextMenu(const QPoint& point)
{
    if (ui->lstQueue->selectedItems().length() == 0)
        return;

    QPoint global_point = ui->lstQueue->mapToGlobal(point);

    QMenu contextMenu(tr("Context menu"), this);

    Action_RemoveFromQueue->setData(ui->lstQueue->row(ui->lstQueue->selectedItems()[0]));
    contextMenu.addAction(Action_RemoveFromQueue);
    contextMenu.exec(global_point);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::ShowContextMenu(const QPoint &point)
{
    if (ui->treeWidget->selectedItems().length() == 0)
        return; // no menu if nothing selected

    QPoint global_point = ui->treeWidget->mapToGlobal(point);
    QMenu contextMenu(tr("Context menu"), this);

    QString const& selected_depot_file = ui->treeWidget->selectedItems().at(0)->data(0, Qt::UserRole).toString();

    if (selected_depot_file.length() == 0)
    {
        QString path;
        QTreeWidgetItem* current = ui->treeWidget->selectedItems().at(0);
        while (current)
        {
            if (path.length())
                path = QDir::separator() + path;
            path = current->text(0) + path;
            current = current->parent();
        }
        Action_ShowInExplorer->setData(path);
        contextMenu.addAction(Action_ShowInExplorer);
        contextMenu.exec(global_point);
        return; // directory.
    }

    FileEntry const& entry = FileMap->operator [](selected_depot_file);

    if (entry.open_by_another.length())
        return; // can't do anything _at all_

    if (entry.exists_in_depot == false)
    {
        // If its not in the depot - we can delete the local file
        // or we can add it to the depot.
        Action_AddToDepot->setData(selected_depot_file);
        contextMenu.addAction(Action_AddToDepot);

        Action_ShowInExplorer->setData(selected_depot_file);
        contextMenu.addAction(Action_ShowInExplorer);

        contextMenu.addSeparator();
        Action_DeleteDisc->setData(selected_depot_file);
        contextMenu.addAction(Action_DeleteDisc);

        contextMenu.exec(global_point);
        return;
    }

    if (entry.local_revision == 0)
    {
        // not subscribed - can subscribe or download
        Action_Subscribe->setData(selected_depot_file);
        contextMenu.addAction(Action_Subscribe);

        Action_Download->setData(selected_depot_file);
        contextMenu.addAction(Action_Download);

        contextMenu.exec(global_point);
        return;
    }

    // at this point its subscribed, but maybe out of date or edited
    if (entry.open_for_edit)
    {
        // if we already have it open, we can revert.
        Action_Revert->setData(selected_depot_file);
        contextMenu.addAction(Action_Revert);

        Action_ShowInExplorer->setData(selected_depot_file);
        contextMenu.addAction(Action_ShowInExplorer);

        contextMenu.exec(global_point);
        return;
    }

    if (entry.head_revision != entry.local_revision)
    {
        // can sync or unsubscribe
        Action_Sync->setData(selected_depot_file);
        contextMenu.addAction(Action_Sync);
        Action_Unsubscribe->setData(selected_depot_file);
        contextMenu.addAction(Action_Unsubscribe);
        Action_ShowInExplorer->setData(selected_depot_file);
        contextMenu.addAction(Action_ShowInExplorer);
    }
    else
    {
        // can edit or unsubscribe
        Action_Edit->setData(selected_depot_file);
        contextMenu.addAction(Action_Edit);
        Action_Unsubscribe->setData(selected_depot_file);
        contextMenu.addAction(Action_Unsubscribe);
        Action_ShowInExplorer->setData(selected_depot_file);
        contextMenu.addAction(Action_ShowInExplorer);
        contextMenu.addSeparator();
        Action_ForceSync->setData(selected_depot_file);
        contextMenu.addAction(Action_ForceSync);

        contextMenu.addSeparator();
        Action_DeleteDepot->setData(selected_depot_file);
        contextMenu.addAction(Action_DeleteDepot);
    }


    contextMenu.exec(global_point);
    return;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::TreeSelectionChanged(const QItemSelection & new_selection, const QItemSelection &)
{
    if (new_selection.length() == 0)
        return; // I don't think there's a way to clear the selection, so just ignore?

    QString const& selected_depot_file = new_selection.indexes().at(0).data(Qt::UserRole).toString();
    if (selected_depot_file.length() == 0)
    {
        // if we select a directory show the repo history.
        ui->lstHistory->clear();
        foreach (QString const& str, *ServerChanges)
        {
            ui->lstHistory->addItem(str);
        }
        return;
    }

    // post to the background thread to grab the history for the selected file.
    QMetaObject::invokeMethod(Tunnel, "get_file_history", Q_ARG(QString, selected_depot_file));

}

static QHash<QString, QTreeWidgetItem*> folder_nodes;
static QHash<QString, QTreeWidgetItem*> file_nodes;

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    file_list_thunker(this),
    FileMap(nullptr),
    ServerChanges(nullptr)
{
    ui->setupUi(this);

    //
    // Actions for the treeview
    //
    Action_Edit = new QAction("Edit", this);
    connect(Action_Edit, SIGNAL(triggered()), this, SLOT(Context_Edit()));
    Action_Revert = new QAction("Revert", this);
    connect(Action_Revert, SIGNAL(triggered()), this, SLOT(Context_Revert()));
    Action_AddToDepot = new QAction("Add To Depot", this);
    connect(Action_AddToDepot, SIGNAL(triggered()), this, SLOT(Context_AddToDepot()));
    Action_DeleteDisc = new QAction("Delete Off Disc", this);
    connect(Action_DeleteDisc, SIGNAL(triggered()), this, SLOT(Context_DeleteDisc()));
    Action_DeleteDepot = new QAction("Delete From Depot", this);
    connect(Action_DeleteDepot, SIGNAL(triggered()), this, SLOT(Context_DeleteDepot()));
    Action_Subscribe = new QAction("Subscribe", this);
    connect(Action_Subscribe, SIGNAL(triggered()), this, SLOT(Context_Subscribe()));
    Action_Unsubscribe = new QAction("Unsubscribe", this);
    connect(Action_Unsubscribe, SIGNAL(triggered()), this, SLOT(Context_Unsubscribe()));
    Action_Download = new QAction("Download", this);
    connect(Action_Download, SIGNAL(triggered()), this, SLOT(Context_Download()));
    Action_Sync = new QAction("Sync", this);
    connect(Action_Sync, SIGNAL(triggered()), this, SLOT(Context_Sync()));
    Action_ForceSync = new QAction("Force Sync", this);
    connect(Action_ForceSync, SIGNAL(triggered()), this, SLOT(Context_ForceSync()));
#ifdef Q_OS_MAC
    Action_ShowInExplorer = new QAction("Show In Finder", this);
#else
    Action_ShowInExplorer = new QAction("Show In Explorer", this);
#endif
    connect(Action_ShowInExplorer, SIGNAL(triggered()), this, SLOT(Context_ShowInExplorer()));

    //
    // Actions for the queue list
    //
    Action_RemoveFromQueue = new QAction("Remove", this);
    connect(Action_RemoveFromQueue, SIGNAL(triggered()), this, SLOT(Context_RemoveFromQueue()));

    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "Midnight Oil Games", "P4SSHQt");

    if ((false))
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
        //PerforcePassword = settings.value("Perforce/Pass", "perforce_pass").toString();
    }

    if (PerforcePassword.length() == 0)
        PerforcePassword = QInputDialog::getText(this, "Perforce Password", "Password:", QLineEdit::Password);

    qDebug() << "Creating SSH Tunnel";

    Tunnel = new SSHTunnel(&file_list_thunker);
    Tunnel->moveToThread(&TunnelThread);

    connect(&TunnelThread, &QThread::finished, Tunnel, &QObject::deleteLater);
    //connect(this, &MainWindow::launch, Tunnel, &SSHTunnel::thread_fn);
    TunnelThread.start();


    QVBoxLayout *mainLayout = new QVBoxLayout();
    mainLayout->addWidget(ui->splitter);

    ui->splitter->setChildrenCollapsible(false);
    ui->centralWidget->setLayout(mainLayout);

    QStringList headers;
    headers << "File" << "Revision" << "State";
    ui->treeWidget->setHeaderLabels(headers);
    ui->treeWidget->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    ui->treeWidget->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    ui->treeWidget->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    ui->treeWidget->header()->setStretchLastSection(false);
    ui->treeWidget->header()->resizeSection(2, 60);
    ui->treeWidget->header()->resizeSection(1, 60);
    ui->treeWidget->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->treeWidget, SIGNAL(customContextMenuRequested(const QPoint &)),
            this, SLOT(ShowContextMenu(const QPoint &)));

    ui->lstQueue->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->lstQueue, SIGNAL(customContextMenuRequested(const QPoint &)),
            this, SLOT(ShowQueueContextMenu(const QPoint &)));

    connect(
      ui->treeWidget->selectionModel(),
      SIGNAL(selectionChanged(const QItemSelection &, const QItemSelection &)),
      SLOT(TreeSelectionChanged(const QItemSelection &, const QItemSelection &))
     );

    ui->lstEdited->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->lstEdited, SIGNAL(customContextMenuRequested(const QPoint &)),
            this, SLOT(ShowEditedContextMenu(const QPoint &)));

    connect(ui->lineEdit, SIGNAL(textEdited(const QString&)), this, SLOT(filter_changed(const QString&)));
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::filter_changed(const QString &text)
{
    {
        QHash<QString, QTreeWidgetItem*>::iterator it = folder_nodes.begin();
        for (; it != folder_nodes.end(); ++it)
        {
            it.value()->setData(0, Qt::UserRole + 1, false);
        }
    }

    {
        QHash<QString, QTreeWidgetItem*>::iterator it = file_nodes.begin();
        for (; it != file_nodes.end(); ++it)
        {
            // if we pass the filter we are visible.
            if (text.length() == 0 ||
                it.key().contains(text, Qt::CaseInsensitive) == true)
            {
                it.value()->setHidden(false);

                // set the crumb for all our parents.
                QTreeWidgetItem* parent = it.value()->parent();
                while (parent)
                {
                    parent->setData(0, Qt::UserRole + 1, true);
                    parent = parent->parent();
                }
            }
            else
                it.value()->setHidden(true);
        }
    }

    {
        // hide any folders not in use.
        QHash<QString, QTreeWidgetItem*>::iterator it = folder_nodes.begin();
        for (; it != folder_nodes.end(); ++it)
        {
            if (it.value()->data(0, Qt::UserRole + 1).toBool() == false)
            {
                it.value()->setHidden(true);
            }
            else
                it.value()->setHidden(false);
        }
    }


}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void FileListThunk::RefreshUIThunk(void* new_file_list, void* new_server_changes)
{
    QMap<QString, FileEntry>* file_list = (QMap<QString, FileEntry>*)new_file_list;
    QStringList* changes = (QStringList*)new_server_changes;
    thunk_to->RefreshUI(file_list, changes);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void FileListThunk::BadPasswordThunk()
{
    thunk_to->BadPassword();
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void FileListThunk::PostFileHistoryThunk(QString file, void* file_history)
{
    QStringList* changes = (QStringList*)file_history;
    thunk_to->PostFileHistory(file, changes);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::PostFileHistory(QString file, QStringList* history)
{
    ui->lstHistory->clear();
    foreach (QString const& str, *history)
    {
        ui->lstHistory->addItem(str);
    }
    delete history;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::BadPassword()
{
    PerforcePassword = QInputDialog::getText(this, "Perforce Password", "Previous password incorrect, enter again:", QLineEdit::Password);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::RefreshUI(QMap<QString, FileEntry>* NewFileMap, QStringList* NewServerChanges)
{
    //
    // Update the UI tree for the depot.
    // For the moment just recreate entirely.
    //
    ui->treeWidget->clear();
    ui->lstEdited->clear();
    ui->lstHistory->clear();
    folder_nodes.clear();
    file_nodes.clear();

    if (ServerChanges)
        delete ServerChanges;
    ServerChanges = NewServerChanges;

    if (FileMap)
        delete FileMap;
    FileMap = NewFileMap;

    foreach (QString const& str, *ServerChanges)
    {
        ui->lstHistory->addItem(str);
    }

    QFileIconProvider icons;
    QIcon FolderIcon = icons.icon(QFileIconProvider::Folder);
    QIcon FileIcon = icons.icon(QFileIconProvider::File);

    QHash<QString, QTreeWidgetItem*> tree;
    QTreeWidgetItem* root = new QTreeWidgetItem((QTreeWidget*)nullptr, QStringList(QString("steph")));
    root->setIcon(0, FolderIcon);

    folder_nodes.insert("steph", root);

    ui->treeWidget->insertTopLevelItem(0, root);
    root->setExpanded(true);

    int out_of_date_count = 0;

    QBrush gray_brush(Qt::gray);
    QBrush red_brush(QColor(255, 160, 160));
    QBrush blue_brush(QColor(160, 160, 255));

    QMap<QString, FileEntry>::const_iterator it = FileMap->constBegin();
    for (; it != FileMap->constEnd(); ++it)
    {
        FileEntry const& entry = it.value();
        QString const& file = it.value().depot_file;

        bool subscribed = false;
        if (entry.local_revision != 0)
            subscribed = true;

        bool out_of_date = subscribed && entry.head_revision != entry.local_revision;
        if (out_of_date)
            out_of_date_count++;

        QStringList sections = file.split("/");
        // the last entry is the filename
        QString accumulator;

        // skip the first entry (don't need "steph" since we did it above)
        for (int i = 1; i < sections.size(); i++)
        {
            // find the tree entry for the path.
            QString current;
            if (accumulator.size())
                current = accumulator + "/" + sections[i];
            else
                current = sections[i];

            QHash<QString, QTreeWidgetItem*>::iterator parent_it = tree.find(current);
            QTreeWidgetItem* parent = nullptr;
            if (parent_it == tree.end())
            {
                // need to create the node.
                QTreeWidgetItem* parent_parent = root;
                if (accumulator.size())
                    parent_parent = tree[accumulator];
                QStringList cols(sections[i]);
                if (i == sections.length()-1)
                {
                    // set up the columns.
                    if (entry.exists_in_depot)
                        cols << (QString::number(entry.local_revision) + "/" + QString::number(entry.head_revision));
                    else
                        cols << "-";

                    if (entry.open_for_edit)
                    {
                        QListWidgetItem* item = new QListWidgetItem(entry.depot_file);
                        ui->lstEdited->addItem(item);
                        cols << "Editing";
                    }
                    else if (entry.open_by_another.length())
                        cols << "Edited by " + entry.open_by_another;
                }
                parent = new QTreeWidgetItem(parent_parent, cols);
                if (i == sections.length()-1)
                {
                    parent->setIcon(0, FileIcon);

                    // this is the file level - set the file path in the data
                    parent->setData(0, Qt::UserRole, file);

                    parent->setTextAlignment(1, Qt::AlignHCenter);
                    parent->setTextAlignment(2, Qt::AlignHCenter);

                    // if the file isn't in the depot, its a gray text
                    if (entry.exists_in_depot == false)
                        parent->setForeground(0, gray_brush);

                    // if the file can't be edited until something is reconciled,
                    // its red.
                    if (subscribed && (out_of_date || entry.open_by_another.length()))
                        parent->setForeground(0, red_brush);

                    if (subscribed == false)
                        parent->setForeground(0, blue_brush);

                    qDebug() << "file: " << file;
                    file_nodes.insert(file, parent);
                }
                else
                {
                    qDebug() << "folder: " << accumulator;

                    folder_nodes.insert(accumulator, parent);
                    parent->setExpanded(true);
                    parent->setIcon(0, FolderIcon);
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
        ui->lblOutOfDate->setText(QString(out_of_date_count) + " files out of date.");

    if (ui->lstEdited->count() == 0)
        ui->btnCommitSelected->setEnabled(false);
    else
    {
        ui->btnCommitSelected->setEnabled(true);
        ui->btnCommitSelected->setText("Commit All");
    }
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
MainWindow::~MainWindow()
{
    delete ui;
    delete FileMap;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
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
void MainWindow::on_btnRefresh_clicked()
{
    QMetaObject::invokeMethod(Tunnel, "retrieve_files", Q_ARG(bool, true));
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::on_lstEdited_itemSelectionChanged()
{
    int count = ui->lstEdited->selectedItems().length();
    if (count == 0)
        ui->btnCommitSelected->setText("Commit All");
    else
        ui->btnCommitSelected->setText("Commit Selected");
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::on_btnCommitSelected_clicked()
{
    if (ui->lstEdited->count() == 0)
        return; // nothing to commit.

    QList<QListWidgetItem *> items = ui->lstEdited->selectedItems();
    if (items.length() == 0)
    {
        for (int i = 0; i < ui->lstEdited->count(); i++)
            items.push_back(ui->lstEdited->item(i));
    }

    QStringList depot_files;
    for (int i = 0; i < items.length(); i++)
    {
        QString file_text = items.at(i)->text();
        depot_files.push_back(file_text);
    }

    SubmitDialog testdialog(depot_files, this);
    testdialog.exec();

    if (testdialog.result() != QDialog::Accepted)
        return;

    QString commit_message = testdialog.commit_msg_edit->toPlainText();
    if (commit_message.length() == 0)
    {
        QMessageBox msgBox;
        msgBox.setText("Requires description.");
        msgBox.setStandardButtons(QMessageBox::Ok);
        msgBox.exec();
        return;
    }

    QueuedAction* qa = new QueuedAction();
    qa->action = QA_Commit;
    qa->relevant_file = commit_message;
    qa->commit_files = depot_files;

    QString ui_text = "<commit> ";
    if (depot_files.length() == 1)
        ui_text += depot_files[0];
    else
        ui_text += QString::number(depot_files.length()) + " files";

    QListWidgetItem* item = new QListWidgetItem(ui_text);
    ui->lstQueue->addItem(item);

    queued_actions.push_back(qa);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::on_btnGetLatest_clicked()
{
    QueuedAction* qa = new QueuedAction();
    qa->action = QA_GetLatest;

    QListWidgetItem* item = new QListWidgetItem("<get latest>");
    ui->lstQueue->addItem(item);

    queued_actions.push_back(qa);
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::on_lstEdited_itemDoubleClicked(QListWidgetItem* item)
{
    QString file_name = item->text();
    FileEntry const& entry = FileMap->operator [](file_name);
    QDesktopServices::openUrl(QUrl("file://" + QDir::fromNativeSeparators(entry.local_file)));
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void MainWindow::on_treeWidget_itemDoubleClicked(QTreeWidgetItem *item, int)
{
    QString file_name = item->data(0, Qt::UserRole).toString();

    if (file_name.length() == 0)
        return; // not an actual file.

    //
    // If we have a copy of he file (ie are subscribed), then we open the file
    // via the associated application.
    //
    FileEntry const& entry = FileMap->operator [](file_name);
    if (entry.exists_in_depot &&
        entry.local_revision != 0)
    {
        QDesktopServices::openUrl(QUrl("file://" + QDir::fromNativeSeparators(entry.local_file)));
    }
    else
    {
        // got a file, ask where to put it.
        QString save_to = QFileDialog::getExistingDirectory(this, "Save To..");

        // send to the background thread to process.
        QMetaObject::invokeMethod(Tunnel, "download_file_to", Q_ARG(QString, file_name), Q_ARG(QString, save_to));
    }
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
SubmitDialog::SubmitDialog(const QStringList &SubmitFiles, QWidget*)
{
    QLabel* list_label = new QLabel;
    list_label->setText("Submitting Files");
    QPlainTextEdit* file_list = new QPlainTextEdit;
    QString submit_text;
    foreach (const QString& file, SubmitFiles)
    {
        submit_text += file;
        submit_text += "\n";
    }
    file_list->setPlainText(submit_text);
    file_list->setEnabled(false);

    QLabel* edit_label = new QLabel;
    edit_label->setText("Change Description");
    commit_msg_edit = new QPlainTextEdit;

    QDialogButtonBox* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok
                                     | QDialogButtonBox::Cancel);

    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    QVBoxLayout *mainLayout = new QVBoxLayout;
    mainLayout->addWidget(list_label);
    mainLayout->addWidget(file_list);
    mainLayout->addWidget(edit_label);
    mainLayout->addWidget(commit_msg_edit);
    mainLayout->addWidget(buttonBox);
    setLayout(mainLayout);
}
