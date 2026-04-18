#include "PythonConsoleWidget.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QScrollBar>
#include <QApplication>
#include <QClipboard>

PythonConsoleWidget::PythonConsoleWidget(QWidget *parent)
    : QPlainTextEdit(parent)
{
    // Configure basic widget properties
    setWordWrapMode(QTextOption::NoWrap);
    setMaximumBlockCount(1000);  // Keep last 1000 lines
    setTabStopDistance(40);       // 4 spaces per tab at typical font width

    // Font setup
    QFont font("Courier", 10);
    font.setStyleStrategy(QFont::PreferAntialias);
    setFont(font);

    // Color scheme
    setStyleSheet(
        "PythonConsoleWidget {"
        "  background-color: #1e1e1e;"
        "  color: #d4d4d4;"
        "  selection-background-color: #264f78;"
        "}"
    );

    // Connect cursor position changes to maintain input area
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &PythonConsoleWidget::onCursorPositionChanged);

    // Show the initial prompt
    showPrompt();
}

void PythonConsoleWidget::appendOutput(const QString &text)
{
    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::End);
    setTextCursor(cursor);

    // Split text into lines
    QStringList lines = text.split('\n', Qt::KeepEmptyParts);
    for (int i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            insertPlainText("\n");
        }
        if (!lines[i].isEmpty()) {
            insertPlainText(lines[i]);
        }
    }

    ensurePromptAtEnd();
}

QString PythonConsoleWidget::getCurrentCommand() const
{
    return m_currentInput;
}

void PythonConsoleWidget::clearCurrentCommand()
{
    m_currentInput.clear();
    m_multilineBuffer.clear();
    m_awaitingContinuation = false;

    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::End);

    // Delete text after the current prompt
    int promptLength = 4;  // "... " or ">>> " are both 4 chars
    cursor.movePosition(QTextCursor::StartOfBlock);
    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::MoveAnchor, promptLength);
    cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();

    setTextCursor(cursor);
}

void PythonConsoleWidget::insertAtCurrentInput(const QString &text)
{
    QTextCursor cursor = textCursor();
    cursor.insertText(text);
    setTextCursor(cursor);
}

void PythonConsoleWidget::showPrompt()
{
    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::End);
    setTextCursor(cursor);

    if (document()->blockCount() > 0) {
        QTextBlock lastBlock = document()->findBlockByLineNumber(document()->blockCount() - 1);
        if (!lastBlock.text().isEmpty()) {
            insertPlainText("\n");
        }
    }

    if (m_awaitingContinuation) {
        insertPlainText(kContinuation);
    } else {
        insertPlainText(kPrompt);
    }

    m_promptBlockNumber = document()->blockCount() - 1;
    cursor = textCursor();
    cursor.movePosition(QTextCursor::End);
    setTextCursor(cursor);

    // Auto-scroll to bottom
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

void PythonConsoleWidget::resetConsole()
{
    clear();
    m_commandHistory.clear();
    m_historyIndex = -1;
    m_currentInput.clear();
    m_multilineBuffer.clear();
    m_awaitingContinuation = false;
    m_promptBlockNumber = -1;
    showPrompt();
}

void PythonConsoleWidget::keyPressEvent(QKeyEvent *event)
{
    QTextCursor cursor = textCursor();

    // Check if cursor is in the input area
    if (!isInInputArea() && event->key() != Qt::Key_End) {
        // Only allow cursor navigation outside input (no text input)
        if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down ||
            event->key() == Qt::Key_PageUp || event->key() == Qt::Key_PageDown ||
            event->key() == Qt::Key_Home) {
            QPlainTextEdit::keyPressEvent(event);
            return;
        }
        moveCursorToInputEnd();
    }

    // Handle special keys with readline-like bindings
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        handleReturnKey();
        return;
    }

    if (event->key() == Qt::Key_Backspace) {
        handleBackspace();
        return;
    }

    // Readline-like key bindings
    if (event->modifiers() & Qt::ControlModifier) {
        if (event->key() == Qt::Key_A) {
            // Ctrl+A: move to start of input
            moveCursorToInputStart();
            return;
        } else if (event->key() == Qt::Key_E) {
            // Ctrl+E: move to end of input
            moveCursorToInputEnd();
            return;
        } else if (event->key() == Qt::Key_K) {
            // Ctrl+K: kill (delete) to end of line
            cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
            setTextCursor(cursor);
            return;
        } else if (event->key() == Qt::Key_U) {
            // Ctrl+U: kill (delete) entire line input
            int promptLength = 4;  // "... " or ">>> " are both 4 chars
            cursor.movePosition(QTextCursor::StartOfBlock);
            cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::MoveAnchor, promptLength);
            cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
            setTextCursor(cursor);
            m_currentInput.clear();
            return;
        }
    }

    // History navigation
    if (event->key() == Qt::Key_Up && m_commandHistory.size() > 0) {
        navigateHistory(-1);
        return;
    } else if (event->key() == Qt::Key_Down && m_commandHistory.size() > 0) {
        navigateHistory(1);
        return;
    }

    // Tab key: insert spaces instead of actual tab
    if (event->key() == Qt::Key_Tab) {
        insertAtCurrentInput("    ");  // 4 spaces
        return;
    }

    // Paste handling (Ctrl+V or Shift+Insert)
    if ((event->modifiers() & Qt::ControlModifier && event->key() == Qt::Key_V) ||
        (event->modifiers() & Qt::ShiftModifier && event->key() == Qt::Key_Insert)) {
        QString pastedText = QApplication::clipboard()->text();
        insertAtCurrentInput(pastedText);
        return;
    }

    // Default key handling for regular input
    if (event->text().length() > 0 && event->text()[0].isPrint()) {
        insertAtCurrentInput(event->text());
        return;
    }

    QPlainTextEdit::keyPressEvent(event);
}

void PythonConsoleWidget::mousePressEvent(QMouseEvent *event)
{
    QPlainTextEdit::mousePressEvent(event);
    // After mouse click, ensure cursor is in the input area if clicking in text
    if (isInInputArea()) {
        return;
    }
    // If click was in output area, don't move cursor on subsequent text entry
}

void PythonConsoleWidget::onCursorPositionChanged()
{
    if (m_blockCursorPositionUpdate) {
        return;
    }

    // Prevent cursor from moving before the prompt
    if (!isInInputArea()) {
        moveCursorToInputEnd();
    }
}

void PythonConsoleWidget::ensurePromptAtEnd()
{
    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::End);

    QTextBlock lastBlock = cursor.block();
    if (!lastBlock.text().trimmed().startsWith(kPrompt) &&
        !lastBlock.text().trimmed().startsWith(kContinuation)) {
        setTextCursor(cursor);
        showPrompt();
    }
}

void PythonConsoleWidget::handleReturnKey()
{
    QString command = extractCommand();

    if (command.isEmpty() && !m_awaitingContinuation) {
        // Empty line in normal mode - just show another prompt
        m_currentInput.clear();
        m_multilineBuffer.clear();
        showPrompt();
        return;
    }

    if (m_awaitingContinuation || command.isEmpty()) {
        // In continuation mode
        if (!command.isEmpty()) {
            m_multilineBuffer += command + "\n";
        }

        // Check if we should continue (simple heuristic: if line ends with ':' or is empty continuation)
        if (command.endsWith(':') || command.endsWith('\\')) {
            m_currentInput = m_multilineBuffer;
            showPrompt();
            return;
        }

        // Check for unbalanced brackets/parens/braces
        QString fullCommand = m_multilineBuffer;
        int openParens = fullCommand.count('(') - fullCommand.count(')');
        int openBrackets = fullCommand.count('[') - fullCommand.count(']');
        int openBraces = fullCommand.count('{') - fullCommand.count('}');

        if (openParens > 0 || openBrackets > 0 || openBraces > 0) {
            m_currentInput = m_multilineBuffer;
            showPrompt();
            return;
        }

        // Command complete, emit it
        m_currentInput = m_multilineBuffer;
        m_awaitingContinuation = false;
        m_multilineBuffer.clear();
    } else {
        // Normal mode, check if this starts a multi-line block
        if (command.endsWith(':') || command.endsWith('\\')) {
            m_multilineBuffer = command + "\n";
            m_awaitingContinuation = true;
            m_currentInput = command;
            showPrompt();
            return;
        }

        // Check for unbalanced brackets
        int openParens = command.count('(') - command.count(')');
        int openBrackets = command.count('[') - command.count(']');
        int openBraces = command.count('{') - command.count('}');

        if (openParens > 0 || openBrackets > 0 || openBraces > 0) {
            m_multilineBuffer = command + "\n";
            m_awaitingContinuation = true;
            m_currentInput = command;
            showPrompt();
            return;
        }
    }

    // Add to history
    if (!m_currentInput.isEmpty() && (m_commandHistory.isEmpty() || m_commandHistory.last() != m_currentInput)) {
        m_commandHistory.append(m_currentInput);
    }
    m_historyIndex = -1;

    // Emit signal for parent to execute the command
    emit returnPressed();

    // Clear current input and show new prompt
    m_currentInput.clear();
    m_multilineBuffer.clear();
    m_awaitingContinuation = false;
    showPrompt();
}

void PythonConsoleWidget::handleBackspace()
{
    if (!isInInputArea()) {
        moveCursorToInputEnd();
        return;
    }

    QTextCursor cursor = textCursor();

    // Don't allow backspace past the prompt
    QTextBlock block = cursor.block();
    int posInBlock = cursor.positionInBlock();
    int promptLength = block.text().startsWith(kContinuation) ? 4 : 4;

    if (posInBlock <= promptLength) {
        return;  // Can't delete past the prompt
    }

    cursor.deletePreviousChar();
    setTextCursor(cursor);

    // Update current input tracking
    m_currentInput = extractCommand();
}

void PythonConsoleWidget::navigateHistory(int direction)
{
    // Save current input if we're at the end
    if (m_historyIndex == -1 && isInInputArea()) {
        m_currentInput = extractCommand();
    }

    int newIndex = m_historyIndex + direction;
    if (newIndex < -1 || newIndex >= m_commandHistory.size()) {
        return;
    }

    m_historyIndex = newIndex;

    // Replace current input with history entry
    clearCurrentCommand();
    if (m_historyIndex >= 0) {
        insertAtCurrentInput(m_commandHistory[m_historyIndex]);
    } else {
        insertAtCurrentInput(m_currentInput);
    }
}

void PythonConsoleWidget::moveCursorToInputStart()
{
    QTextCursor cursor = textCursor();
    int promptLength = 4;  // "... " or ">>> " are both 4 chars
    cursor.movePosition(QTextCursor::StartOfBlock);
    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::MoveAnchor, promptLength);
    setTextCursor(cursor);
}

void PythonConsoleWidget::moveCursorToInputEnd()
{
    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::End);
    setTextCursor(cursor);
}

QString PythonConsoleWidget::extractCommand()
{
    QTextCursor cursor = textCursor();
    QTextBlock block = cursor.block();
    QString blockText = block.text();

    // Remove prompt
    if (blockText.startsWith(kContinuation)) {
        return blockText.mid(4);  // Length of "... "
    } else if (blockText.startsWith(kPrompt)) {
        return blockText.mid(4);  // Length of ">>> "
    }

    return blockText;
}

bool PythonConsoleWidget::isInInputArea() const
{
    QTextCursor cursor = textCursor();
    QTextBlock block = cursor.block();
    QString blockText = block.text();

    // Check if current line has a prompt
    return blockText.startsWith(kPrompt) || blockText.startsWith(kContinuation);
}

void PythonConsoleWidget::insertPrompt()
{
    if (m_awaitingContinuation) {
        insertPlainText(kContinuation);
    } else {
        insertPlainText(kPrompt);
    }
}

void PythonConsoleWidget::applyConsoleFormatting()
{
    // This could be expanded to apply syntax highlighting
    // For now, the stylesheet handles the basic colors
}
