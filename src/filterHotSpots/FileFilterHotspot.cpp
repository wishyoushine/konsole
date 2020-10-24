/*
    Copyright 2007-2008 by Robert Knight <robertknight@gmail.com>
    Copyright 2020 by Tomaz Canabrava <tcanabrava@gmail.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
    02110-1301  USA.
*/

#include "FileFilterHotspot.h"

#include <QApplication>
#include <QAction>
#include <QBuffer>
#include <QClipboard>
#include <QMenu>
#include <QTimer>
#include <QToolTip>
#include <QMouseEvent>
#include <QKeyEvent>

#include <KIO/CommandLauncherJob>
#include <KIO/ApplicationLauncherJob>
#include <KIO/JobUiDelegate>
#include <KIO/OpenUrlJob>
#include <KLocalizedString>
#include <KFileItemListProperties>
#include <KMessageBox>
#include <KShell>

#include "konsoledebug.h"
#include "KonsoleSettings.h"
#include "profile/Profile.h"
#include "session/SessionManager.h"
#include "terminalDisplay/TerminalDisplay.h"

using namespace Konsole;


FileFilterHotSpot::FileFilterHotSpot(int startLine, int startColumn, int endLine, int endColumn,
                                     const QStringList &capturedTexts, const QString &filePath,
                                     Session *session)
  : RegExpFilterHotSpot(startLine, startColumn, endLine, endColumn, capturedTexts),
    _filePath(filePath),
    _session(session)
{
    setType(Link);
}

void FileFilterHotSpot::activate(QObject *)
{
    auto openUrl = [](const QString filePath) {
        auto *job = new KIO::OpenUrlJob(QUrl::fromLocalFile(filePath));
        job->setUiDelegate(new KIO::JobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, QApplication::activeWindow()));
        job->setRunExecutables(false); // Always open, e.g. shell scripts, as text
        job->start();
    };

    // Output of e.g.:
    // - grep with line numbers: "path/to/some/file:123:"
    // - compiler errors with line/column numbers: "/path/to/file.cpp:123:123:"
    // - ctest failing unit tests: "/path/to/file(204)"
    const auto re(QRegularExpression(QStringLiteral(R"foo((?:[:\(]?(\d+)(?::?|\)\]))(?:(\d+):)?$)foo")));
    QRegularExpressionMatch match = re.match(_filePath);
    if (match.hasMatch()) {
        // The file path without the ":123" ... etc bits
        const QString path = _filePath.mid(0, match.capturedStart(0));

        if (!_session) {
            openUrl(path);
            return;
        }

        Profile::Ptr profile = SessionManager::instance()->sessionProfile(_session);
        QString editorCmd = profile->textEditorCmd();

        // If the cmd is empty or PATH and LINE don't exist, then the command
        // is malformed, ignore it and open with the default editor
        // TODO: show an error message to the user?
        if (editorCmd.isEmpty()
            || ! (editorCmd.contains(QLatin1String("PATH")) && editorCmd.contains(QLatin1String("LINE")))) {
            openUrl(path);
            return;
        }

        editorCmd.replace(QLatin1String("LINE"), match.captured(1));

        const QString col = match.captured(2);

        editorCmd.replace(QLatin1String("COLUMN"), !col.isEmpty() ? col : QLatin1String("0"));

        editorCmd.replace(QLatin1String("PATH"), path);

        qCDebug(KonsoleDebug) << "editorCmd:" << editorCmd;

        KService::Ptr service(new KService(QString(), editorCmd, QString()));
        // ApplicationLauncherJob is better at reporting errors to the user than
        // CommandLauncherJob; no need to call job->setUrls() because the url is
        // already part of editorCmd
        auto *job = new KIO::ApplicationLauncherJob(service);
        connect(job, &KJob::result, this, [this, path, job, openUrl]() {
            if (job->error()) {
                // TODO: use KMessageWidget (like the "terminal is read-only" message)
                KMessageBox::sorry(QApplication::activeWindow(),
                                   i18n("Could not open file with the text editor specified in the profile settings;\n"
                                        "it will be opened with the system default editor."));

                openUrl(path);
            }
        });
        job->start();

        return;
    }

    // There was no match, i.e. regular url "path/to/file", open it with
    // the system default editor
    // In case the regex above didn't match for any reason, clean up the file path
    QString path(_filePath);
    path.remove(QRegularExpression(QStringLiteral(R"foo((:\d+[:]?$))foo"), QRegularExpression::DontCaptureOption));
    openUrl(path);
}


FileFilterHotSpot::~FileFilterHotSpot() = default;

QList<QAction *> FileFilterHotSpot::actions()
{
    QAction *action = new QAction(i18n("Copy Location"), this);
    action->setIcon(QIcon::fromTheme(QStringLiteral("edit-copy")));
    connect(action, &QAction::triggered, this, [this] {
        QGuiApplication::clipboard()->setText(_filePath);
    });
    return {action};
}

void FileFilterHotSpot::setupMenu(QMenu *menu)
{
    // We are reusing the QMenu, but we need to update the actions anyhow.
    // Remove the 'Open with' actions from it, then add the new ones.
    QList<QAction*> toDelete;
    for (auto *action : menu->actions()) {
        if (action->text().toLower().remove(QLatin1Char('&')).contains(i18n("open with"))) {
            toDelete.append(action);
        }
    }
    qDeleteAll(toDelete);

    const KFileItem fileItem(QUrl::fromLocalFile(_filePath));
    const KFileItemList itemList({fileItem});
    const KFileItemListProperties itemProperties(itemList);
    _menuActions.setParent(this);
    _menuActions.setItemListProperties(itemProperties);
    _menuActions.addOpenWithActionsTo(menu);

    // Here we added the actions to the last part of the menu, but we need to move them up.
    // TODO: As soon as addOpenWithActionsTo accepts a index, change this.
    // https://bugs.kde.org/show_bug.cgi?id=423765
    QAction *firstAction = menu->actions().at(0);
    for (auto *action : menu->actions()) {
        if (action->text().toLower().remove(QLatin1Char('&')).contains(i18n("open with"))) {
            menu->removeAction(action);
            menu->insertAction(firstAction, action);
        }
    }
    auto *separator = new QAction(this);
    separator->setSeparator(true);
    menu->insertAction(firstAction, separator);
}

// Static variables for the HotSpot
bool FileFilterHotSpot::_canGenerateThumbnail = false;
QPointer<KIO::PreviewJob> FileFilterHotSpot::_previewJob;

void FileFilterHotSpot::requestThumbnail(Qt::KeyboardModifiers modifiers, const QPoint &pos) {
    _canGenerateThumbnail = true;
    _eventModifiers = modifiers;
    _eventPos = pos;

    // Defer the real creation of the thumbnail by a few msec.
    QTimer::singleShot(250, this, [this]{
        thumbnailRequested();
    });
}

void FileFilterHotSpot::stopThumbnailGeneration()
{
    _canGenerateThumbnail = false;
    if (_previewJob != nullptr) {
        _previewJob->deleteLater();
        QToolTip::hideText();
    }
}

void FileFilterHotSpot::showThumbnail(const KFileItem& item, const QPixmap& preview)
{
    if (!_canGenerateThumbnail) {
        return;
    }
    _thumbnailFinished = true;
    Q_UNUSED(item)
    QByteArray data;
    QBuffer buffer(&data);
    preview.save(&buffer, "PNG", 100);

    const auto tooltipString = QStringLiteral("<img src='data:image/png;base64, %0'>")
        .arg(QString::fromLocal8Bit(data.toBase64()));

    QToolTip::showText(_thumbnailPos, tooltipString, qApp->focusWidget());
}

void FileFilterHotSpot::thumbnailRequested() {
    if (!_canGenerateThumbnail) {
        return;
    }

    auto *settings = KonsoleSettings::self();

    Qt::KeyboardModifiers modifiers = settings->thumbnailCtrl() ? Qt::ControlModifier : Qt::NoModifier;
    modifiers |= settings->thumbnailAlt() ? Qt::AltModifier : Qt::NoModifier;
    modifiers |= settings->thumbnailShift() ? Qt::ShiftModifier : Qt::NoModifier;

    if (_eventModifiers != modifiers) {
        return;
    }

    _thumbnailPos = QPoint(_eventPos.x() + 100, _eventPos.y() - settings->thumbnailSize() / 2);

    const int size = KonsoleSettings::thumbnailSize();
    if (_previewJob != nullptr) {
        _previewJob->deleteLater();
    }

    _thumbnailFinished = false;

    // Show a "Loading" if Preview takes a long time.
    QTimer::singleShot(10, this, [this]{
        if (_previewJob == nullptr) {
            return;
        }
        if (!_thumbnailFinished) {
            QToolTip::showText(_thumbnailPos, i18n("Generating Thumbnail"), qApp->focusWidget());
        }
    });

    _previewJob = new KIO::PreviewJob(KFileItemList({fileItem()}), QSize(size, size));
    connect(_previewJob, &KIO::PreviewJob::gotPreview, this, &FileFilterHotSpot::showThumbnail);
    connect(_previewJob, &KIO::PreviewJob::failed, this, []{
        qCDebug(KonsoleDebug) << "Error generating the preview" << _previewJob->errorString();
        QToolTip::hideText();
    });

    _previewJob->setAutoDelete(true);
    _previewJob->start();
}

KFileItem FileFilterHotSpot::fileItem() const
{
    return KFileItem(QUrl::fromLocalFile(_filePath));
}

void FileFilterHotSpot::mouseEnterEvent(TerminalDisplay *td, QMouseEvent *ev)
{
    HotSpot::mouseEnterEvent(td, ev);
    requestThumbnail(ev->modifiers(), ev->globalPos());
}

void FileFilterHotSpot::mouseLeaveEvent(TerminalDisplay *td, QMouseEvent *ev)
{
    HotSpot::mouseLeaveEvent(td, ev);
    stopThumbnailGeneration();
}

void Konsole::FileFilterHotSpot::keyPressEvent(Konsole::TerminalDisplay* td, QKeyEvent* ev)
{
    HotSpot::keyPressEvent(td, ev);
    requestThumbnail(ev->modifiers(), QCursor::pos());
}
