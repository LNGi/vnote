#include <QtWidgets>
#include <QVector>
#include <QDebug>
#include "vedit.h"
#include "vnote.h"
#include "vconfigmanager.h"
#include "vtoc.h"
#include "utils/vutils.h"
#include "veditoperations.h"
#include "dialog/vfindreplacedialog.h"
#include "vedittab.h"

extern VConfigManager vconfig;
extern VNote *g_vnote;

void VEditConfig::init(const QFontMetrics &p_metric)
{
    if (vconfig.getTabStopWidth() > 0) {
        m_tabStopWidth = vconfig.getTabStopWidth() * p_metric.width(' ');
    } else {
        m_tabStopWidth = 0;
    }

    m_expandTab = vconfig.getIsExpandTab();

    if (m_expandTab && (vconfig.getTabStopWidth() > 0)) {
        m_tabSpaces = QString(vconfig.getTabStopWidth(), ' ');
    } else {
        m_tabSpaces = "\t";
    }

    m_enableVimMode = vconfig.getEnableVimMode();

    m_cursorLineBg = QColor(vconfig.getEditorCurrentLineBg());

    m_highlightWholeBlock = m_enableVimMode;
}

VEdit::VEdit(VFile *p_file, QWidget *p_parent)
    : QTextEdit(p_parent), m_file(p_file), m_editOps(NULL)
{
    const int labelTimerInterval = 500;
    const int extraSelectionHighlightTimer = 500;
    const int labelSize = 64;

    m_selectedWordColor = QColor("Yellow");
    m_searchedWordColor = QColor(g_vnote->getColorFromPalette("Green4"));
    m_trailingSpaceColor = QColor(vconfig.getEditorTrailingSpaceBackground());

    QPixmap wrapPixmap(":/resources/icons/search_wrap.svg");
    m_wrapLabel = new QLabel(this);
    m_wrapLabel->setPixmap(wrapPixmap.scaled(labelSize, labelSize));
    m_wrapLabel->hide();
    m_labelTimer = new QTimer(this);
    m_labelTimer->setSingleShot(true);
    m_labelTimer->setInterval(labelTimerInterval);
    connect(m_labelTimer, &QTimer::timeout,
            this, &VEdit::labelTimerTimeout);

    m_highlightTimer = new QTimer(this);
    m_highlightTimer->setSingleShot(true);
    m_highlightTimer->setInterval(extraSelectionHighlightTimer);
    connect(m_highlightTimer, &QTimer::timeout,
            this, &VEdit::doHighlightExtraSelections);

    connect(document(), &QTextDocument::modificationChanged,
            (VFile *)m_file, &VFile::setModified);

    m_extraSelections.resize((int)SelectionId::MaxSelection);

    updateFontAndPalette();

    updateConfig();

    connect(this, &VEdit::cursorPositionChanged,
            this, &VEdit::handleCursorPositionChanged);

    connect(this, &VEdit::selectionChanged,
            this, &VEdit::highlightSelectedWord);
}

VEdit::~VEdit()
{
    if (m_file) {
        disconnect(document(), &QTextDocument::modificationChanged,
                   (VFile *)m_file, &VFile::setModified);
    }
}

void VEdit::updateConfig()
{
    m_config.init(QFontMetrics(font()));

    if (m_config.m_tabStopWidth > 0) {
        setTabStopWidth(m_config.m_tabStopWidth);
    }

    emit configUpdated();
}

void VEdit::beginEdit()
{
    updateFontAndPalette();

    updateConfig();

    setReadOnly(false);
    setModified(false);
}

void VEdit::endEdit()
{
    setReadOnly(true);
}

void VEdit::saveFile()
{
    if (!document()->isModified()) {
        return;
    }
    m_file->setContent(toHtml());
    document()->setModified(false);
}

void VEdit::reloadFile()
{
    setHtml(m_file->getContent());
    setModified(false);
}

void VEdit::scrollToLine(int p_lineNumber)
{
    Q_ASSERT(p_lineNumber >= 0);

    // Move the cursor to the end first
    moveCursor(QTextCursor::End);
    QTextCursor cursor(document()->findBlockByLineNumber(p_lineNumber));
    cursor.movePosition(QTextCursor::EndOfBlock);
    setTextCursor(cursor);
}

bool VEdit::isModified() const
{
    return document()->isModified();
}

void VEdit::setModified(bool p_modified)
{
    document()->setModified(p_modified);
    if (m_file) {
        m_file->setModified(p_modified);
    }
}

void VEdit::insertImage()
{
    if (m_editOps) {
        m_editOps->insertImage();
    }
}

bool VEdit::peekText(const QString &p_text, uint p_options)
{
    static int startPos = textCursor().selectionStart();
    static int lastPos = startPos;
    bool found = false;

    if (p_text.isEmpty()) {
        // Clear previous selection
        QTextCursor cursor = textCursor();
        cursor.clearSelection();
        cursor.setPosition(startPos);
        setTextCursor(cursor);
    } else {
        QTextCursor cursor = textCursor();
        int curPos = cursor.selectionStart();
        if (curPos != lastPos) {
            // Cursor has been moved. Just start at current potition.
            startPos = curPos;
            lastPos = curPos;
        } else {
            cursor.setPosition(startPos);
            setTextCursor(cursor);
        }
    }
    bool wrapped = false;
    found = findTextHelper(p_text, p_options, true, wrapped);
    if (found) {
        lastPos = textCursor().selectionStart();
        found = true;
    }
    return found;
}

// Use QTextEdit::find() instead of QTextDocument::find() because the later has
// bugs in searching backward.
bool VEdit::findTextHelper(const QString &p_text, uint p_options,
                           bool p_forward, bool &p_wrapped)
{
    p_wrapped = false;
    bool found = false;

    // Options
    QTextDocument::FindFlags findFlags;
    bool caseSensitive = false;
    if (p_options & FindOption::CaseSensitive) {
        findFlags |= QTextDocument::FindCaseSensitively;
        caseSensitive = true;
    }
    if (p_options & FindOption::WholeWordOnly) {
        findFlags |= QTextDocument::FindWholeWords;
    }
    if (!p_forward) {
        findFlags |= QTextDocument::FindBackward;
    }
    // Use regular expression
    bool useRegExp = false;
    QRegExp exp;
    if (p_options & FindOption::RegularExpression) {
        useRegExp = true;
        exp = QRegExp(p_text,
                      caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
    }
    QTextCursor cursor = textCursor();
    while (!found) {
        if (useRegExp) {
            found = find(exp, findFlags);
        } else {
            found = find(p_text, findFlags);
        }
        if (p_wrapped) {
            if (!found) {
                setTextCursor(cursor);
            }
            break;
        }
        if (!found) {
            // Wrap to the other end of the document to search again.
            p_wrapped = true;
            QTextCursor wrapCursor = textCursor();
            wrapCursor.clearSelection();
            if (p_forward) {
                wrapCursor.movePosition(QTextCursor::Start, QTextCursor::MoveAnchor);
            } else {
                wrapCursor.movePosition(QTextCursor::End, QTextCursor::MoveAnchor);
            }
            setTextCursor(wrapCursor);
        }
    }
    return found;
}

QList<QTextCursor> VEdit::findTextAll(const QString &p_text, uint p_options)
{
    QList<QTextCursor> results;
    if (p_text.isEmpty()) {
        return results;
    }
    // Options
    QTextDocument::FindFlags findFlags;
    bool caseSensitive = false;
    if (p_options & FindOption::CaseSensitive) {
        findFlags |= QTextDocument::FindCaseSensitively;
        caseSensitive = true;
    }
    if (p_options & FindOption::WholeWordOnly) {
        findFlags |= QTextDocument::FindWholeWords;
    }
    // Use regular expression
    bool useRegExp = false;
    QRegExp exp;
    if (p_options & FindOption::RegularExpression) {
        useRegExp = true;
        exp = QRegExp(p_text,
                      caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
    }
    int startPos = 0;
    QTextCursor cursor;
    QTextDocument *doc = document();
    while (true) {
        if (useRegExp) {
            cursor = doc->find(exp, startPos, findFlags);
        } else {
            cursor = doc->find(p_text, startPos, findFlags);
        }
        if (cursor.isNull()) {
            break;
        } else {
            results.append(cursor);
            startPos = cursor.selectionEnd();
        }
    }
    return results;
}

bool VEdit::findText(const QString &p_text, uint p_options, bool p_forward)
{
    bool found = false;
    if (p_text.isEmpty()) {
        QTextCursor cursor = textCursor();
        cursor.clearSelection();
        setTextCursor(cursor);
    } else {
        bool wrapped = false;
        found = findTextHelper(p_text, p_options, p_forward, wrapped);
        if (found) {
            if (wrapped) {
                showWrapLabel();
            }
            highlightSearchedWord(p_text, p_options);
        } else {
            // Simply clear previous highlight.
            highlightSearchedWord("", p_options);
        }
    }
    qDebug() << "findText" << p_text << p_options << p_forward
             << (found ? "Found" : "NotFound");
    return found;
}

void VEdit::replaceText(const QString &p_text, uint p_options,
                        const QString &p_replaceText, bool p_findNext)
{
    QTextCursor cursor = textCursor();
    if (cursor.hasSelection()) {
        // Replace occurs only if the selected text matches @p_text with @p_options.
        QTextCursor tmpCursor = cursor;
        tmpCursor.setPosition(tmpCursor.selectionStart());
        tmpCursor.clearSelection();
        setTextCursor(tmpCursor);
        bool wrapped = false;
        bool found = findTextHelper(p_text, p_options, true, wrapped);
        bool matched = false;
        if (found) {
            tmpCursor = textCursor();
            matched = (cursor.selectionStart() == tmpCursor.selectionStart())
                      && (cursor.selectionEnd() == tmpCursor.selectionEnd());
        }
        if (matched) {
            cursor.beginEditBlock();
            cursor.removeSelectedText();
            cursor.insertText(p_replaceText);
            cursor.endEditBlock();
            setTextCursor(cursor);
        } else {
            setTextCursor(cursor);
        }
    }
    if (p_findNext) {
        findText(p_text, p_options, true);
    }
}

void VEdit::replaceTextAll(const QString &p_text, uint p_options,
                           const QString &p_replaceText)
{
    // Replace from the start to the end and resotre the cursor.
    QTextCursor cursor = textCursor();
    int nrReplaces = 0;
    QTextCursor tmpCursor = cursor;
    tmpCursor.setPosition(0);
    setTextCursor(tmpCursor);
    while (true) {
        bool wrapped = false;
        bool found = findTextHelper(p_text, p_options, true, wrapped);
        if (!found) {
            break;
        } else {
            if (wrapped) {
                // Wrap back.
                break;
            }
            nrReplaces++;
            tmpCursor = textCursor();
            tmpCursor.beginEditBlock();
            tmpCursor.removeSelectedText();
            tmpCursor.insertText(p_replaceText);
            tmpCursor.endEditBlock();
            setTextCursor(tmpCursor);
        }
    }
    // Restore cursor position.
    cursor.clearSelection();
    setTextCursor(cursor);
    qDebug() << "replace all" << nrReplaces << "occurences";
}

void VEdit::showWrapLabel()
{
    int labelW = m_wrapLabel->width();
    int labelH = m_wrapLabel->height();
    int x = (width() - labelW) / 2;
    int y = (height() - labelH) / 2;
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    m_wrapLabel->move(x, y);
    m_wrapLabel->show();
    m_labelTimer->stop();
    m_labelTimer->start();
}

void VEdit::labelTimerTimeout()
{
    m_wrapLabel->hide();
}

void VEdit::updateFontAndPalette()
{
    setFont(vconfig.getBaseEditFont());
    setPalette(vconfig.getBaseEditPalette());
}

void VEdit::highlightExtraSelections(bool p_now)
{
    m_highlightTimer->stop();
    if (p_now) {
        doHighlightExtraSelections();
    } else {
        m_highlightTimer->start();
    }
}

void VEdit::doHighlightExtraSelections()
{
    int nrExtra = m_extraSelections.size();
    Q_ASSERT(nrExtra == (int)SelectionId::MaxSelection);
    QList<QTextEdit::ExtraSelection> extraSelects;
    for (int i = 0; i < nrExtra; ++i) {
        extraSelects.append(m_extraSelections[i]);
    }

    setExtraSelections(extraSelects);
}

void VEdit::highlightCurrentLine()
{
    QList<QTextEdit::ExtraSelection> &selects = m_extraSelections[(int)SelectionId::CurrentLine];
    if (vconfig.getHighlightCursorLine() && !isReadOnly()) {
        // Need to highlight current line.
        selects.clear();

        // A long block maybe splited into multiple visual lines.
        QTextEdit::ExtraSelection select;
        select.format.setBackground(m_config.m_cursorLineBg);
        select.format.setProperty(QTextFormat::FullWidthSelection, true);

        QTextCursor cursor = textCursor();
        if (m_config.m_highlightWholeBlock) {
            cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::MoveAnchor, 1);
            QTextBlock block = cursor.block();
            int blockEnd = block.position() + block.length();
            int pos = -1;
            while (cursor.position() < blockEnd && pos != cursor.position()) {
                QTextEdit::ExtraSelection newSelect = select;
                newSelect.cursor = cursor;
                selects.append(newSelect);

                pos = cursor.position();
                cursor.movePosition(QTextCursor::Down, QTextCursor::MoveAnchor, 1);
            }
        } else {
            cursor.clearSelection();
            select.cursor = cursor;
            selects.append(select);
        }
    } else {
        // Need to clear current line highlight.
        if (selects.isEmpty()) {
            return;
        }

        selects.clear();
    }

    highlightExtraSelections(true);
}

void VEdit::setReadOnly(bool p_ro)
{
    QTextEdit::setReadOnly(p_ro);
    highlightCurrentLine();
}

void VEdit::highlightSelectedWord()
{
    QList<QTextEdit::ExtraSelection> &selects = m_extraSelections[(int)SelectionId::SelectedWord];
    if (!vconfig.getHighlightSelectedWord()) {
        if (!selects.isEmpty()) {
            selects.clear();
            highlightExtraSelections(true);
        }

        return;
    }

    QString text = textCursor().selectedText().trimmed();
    if (text.isEmpty() || wordInSearchedSelection(text)) {
        selects.clear();
        highlightExtraSelections(true);
        return;
    }

    QTextCharFormat format;
    format.setBackground(m_selectedWordColor);
    highlightTextAll(text, FindOption::CaseSensitive, SelectionId::SelectedWord,
                     format);
}

// Do not highlight trailing spaces with current cursor right behind.
static void trailingSpaceFilter(VEdit *p_editor, QList<QTextEdit::ExtraSelection> &p_result)
{
    QTextCursor cursor = p_editor->textCursor();
    if (!cursor.atBlockEnd()) {
        return;
    }

    int cursorPos = cursor.position();
    for (auto it = p_result.begin(); it != p_result.end(); ++it) {
        if (it->cursor.selectionEnd() == cursorPos) {
            p_result.erase(it);

            // There will be only one.
            return;
        }
    }
}

void VEdit::highlightTrailingSpace()
{
    if (!vconfig.getEnableTrailingSpaceHighlight()) {
        QList<QTextEdit::ExtraSelection> &selects = m_extraSelections[(int)SelectionId::TrailingSapce];
        if (!selects.isEmpty()) {
            selects.clear();
            highlightExtraSelections(true);
        }
        return;
    }

    QTextCharFormat format;
    format.setBackground(m_trailingSpaceColor);
    QString text("\\s+$");
    highlightTextAll(text, FindOption::RegularExpression,
                     SelectionId::TrailingSapce, format,
                     trailingSpaceFilter);
}

bool VEdit::wordInSearchedSelection(const QString &p_text)
{
    QString text = p_text.trimmed();
    QList<QTextEdit::ExtraSelection> &selects = m_extraSelections[(int)SelectionId::SearchedKeyword];
    for (int i = 0; i < selects.size(); ++i) {
        QString searchedWord = selects[i].cursor.selectedText();
        if (text == searchedWord.trimmed()) {
            return true;
        }
    }
    return false;
}

void VEdit::highlightTextAll(const QString &p_text, uint p_options,
                             SelectionId p_id, QTextCharFormat p_format,
                             void (*p_filter)(VEdit *, QList<QTextEdit::ExtraSelection> &))
{
    QList<QTextEdit::ExtraSelection> &selects = m_extraSelections[(int)p_id];
    if (!p_text.isEmpty()) {
        selects.clear();

        QList<QTextCursor> occurs = findTextAll(p_text, p_options);
        for (int i = 0; i < occurs.size(); ++i) {
            QTextEdit::ExtraSelection select;
            select.format = p_format;
            select.cursor = occurs[i];
            selects.append(select);
        }
    } else {
        if (selects.isEmpty()) {
            return;
        }
        selects.clear();
    }

    if (p_filter) {
        p_filter(this, selects);
    }

    highlightExtraSelections();
}

void VEdit::highlightSearchedWord(const QString &p_text, uint p_options)
{
    QList<QTextEdit::ExtraSelection> &selects = m_extraSelections[(int)SelectionId::SearchedKeyword];
    if (!vconfig.getHighlightSearchedWord() || p_text.isEmpty()) {
        if (!selects.isEmpty()) {
            selects.clear();
            highlightExtraSelections(true);
        }

        return;
    }

    QTextCharFormat format;
    format.setBackground(m_searchedWordColor);
    highlightTextAll(p_text, p_options, SelectionId::SearchedKeyword, format);
}

void VEdit::clearSearchedWordHighlight()
{
    QList<QTextEdit::ExtraSelection> &selects = m_extraSelections[(int)SelectionId::SearchedKeyword];
    selects.clear();
    highlightExtraSelections(true);
}

void VEdit::contextMenuEvent(QContextMenuEvent *p_event)
{
    QMenu *menu = createStandardContextMenu();
    menu->setToolTipsVisible(true);

    const QList<QAction *> actions = menu->actions();

    if (!textCursor().hasSelection()) {
        VEditTab *editTab = dynamic_cast<VEditTab *>(parent());
        V_ASSERT(editTab);
        if (editTab->isEditMode()) {
            QAction *saveExitAct = new QAction(QIcon(":/resources/icons/save_exit.svg"),
                                               tr("&Save Changes And Read"), this);
            saveExitAct->setToolTip(tr("Save changes and exit edit mode"));
            connect(saveExitAct, &QAction::triggered,
                    this, &VEdit::handleSaveExitAct);

            QAction *discardExitAct = new QAction(QIcon(":/resources/icons/discard_exit.svg"),
                                                  tr("&Discard Changes And Read"), this);
            discardExitAct->setToolTip(tr("Discard changes and exit edit mode"));
            connect(discardExitAct, &QAction::triggered,
                    this, &VEdit::handleDiscardExitAct);

            menu->insertAction(actions.isEmpty() ? NULL : actions[0], discardExitAct);
            menu->insertAction(discardExitAct, saveExitAct);
            if (!actions.isEmpty()) {
                menu->insertSeparator(actions[0]);
            }
        } else if (m_file->isModifiable()) {
            // HTML.
            QAction *editAct= new QAction(QIcon(":/resources/icons/edit_note.svg"),
                                          tr("&Edit"), this);
            editAct->setToolTip(tr("Edit current note"));
            connect(editAct, &QAction::triggered,
                    this, &VEdit::handleEditAct);
            menu->insertAction(actions.isEmpty() ? NULL : actions[0], editAct);
            // actions does not contain editAction.
            if (!actions.isEmpty()) {
                menu->insertSeparator(actions[0]);
            }
        }
    }

    menu->exec(p_event->globalPos());
    delete menu;
}

void VEdit::handleSaveExitAct()
{
    emit saveAndRead();
}

void VEdit::handleDiscardExitAct()
{
    emit discardAndRead();
}

void VEdit::handleEditAct()
{
    emit editNote();
}

VFile *VEdit::getFile() const
{
    return m_file;
}

void VEdit::handleCursorPositionChanged()
{
    static QTextCursor lastCursor;

    QTextCursor cursor = textCursor();
    if (lastCursor.isNull() || cursor.blockNumber() != lastCursor.blockNumber()) {
        highlightCurrentLine();
        highlightTrailingSpace();
    } else {
        // Judge whether we have trailing space at current line.
        QString text = cursor.block().text();
        if (text.rbegin()->isSpace()) {
            highlightTrailingSpace();
        }

        // Handle word-wrap in one block.
        // Highlight current line if in different visual line.
        if ((lastCursor.positionInBlock() - lastCursor.columnNumber()) !=
            (cursor.positionInBlock() - cursor.columnNumber())) {
            highlightCurrentLine();
        }
    }

    lastCursor = cursor;
}

VEditConfig &VEdit::getConfig()
{
    return m_config;
}

void VEdit::mousePressEvent(QMouseEvent *p_event)
{
    if (p_event->button() == Qt::LeftButton
        && p_event->modifiers() == Qt::ControlModifier
        && !textCursor().hasSelection()) {
        m_oriMouseX = p_event->x();
        m_oriMouseY = p_event->y();
        m_readyToScroll = true;
        m_mouseMoveScrolled = false;
        p_event->accept();
        return;
    }

    m_readyToScroll = false;
    m_mouseMoveScrolled = false;

    QTextEdit::mousePressEvent(p_event);
}

void VEdit::mouseReleaseEvent(QMouseEvent *p_event)
{
    if (m_mouseMoveScrolled || m_readyToScroll) {
        viewport()->setCursor(Qt::IBeamCursor);
        m_readyToScroll = false;
        m_mouseMoveScrolled = false;
        p_event->accept();
        return;
    }

    m_readyToScroll = false;
    m_mouseMoveScrolled = false;

    QTextEdit::mouseReleaseEvent(p_event);
}

void VEdit::mouseMoveEvent(QMouseEvent *p_event)
{
    const int threshold = 5;

    if (m_readyToScroll) {
        int deltaX = p_event->x() - m_oriMouseX;
        int deltaY = p_event->y() - m_oriMouseY;

        if (qAbs(deltaX) >= threshold || qAbs(deltaY) >= threshold) {
            m_oriMouseX = p_event->x();
            m_oriMouseY = p_event->y();

            if (!m_mouseMoveScrolled) {
                m_mouseMoveScrolled = true;
                viewport()->setCursor(Qt::SizeAllCursor);
            }

            QScrollBar *verBar = verticalScrollBar();
            QScrollBar *horBar = horizontalScrollBar();
            if (verBar->isVisible()) {
                verBar->setValue(verBar->value() - deltaY);
            }

            if (horBar->isVisible()) {
                horBar->setValue(horBar->value() - deltaX);
            }
        }

        p_event->accept();
        return;
    }

    QTextEdit::mouseMoveEvent(p_event);
}

void VEdit::requestUpdateVimStatus()
{
    if (m_editOps) {
        m_editOps->requestUpdateVimStatus();
    } else {
        emit vimStatusUpdated(NULL);
    }
}

bool VEdit::jumpTitle(bool p_forward, int p_relativeLevel, int p_repeat)
{
    Q_UNUSED(p_forward);
    Q_UNUSED(p_relativeLevel);
    Q_UNUSED(p_repeat);
    return false;
}
