#include "PythonScintillaEditor.h"

#include <Qsci/qscilexerpython.h>
#include <QKeyEvent>
#include <QTextCursor>
#include <QApplication>
#include <QClipboard>
#include <QFontMetrics>

PythonScintillaEditor::PythonScintillaEditor(QWidget *parent)
    : QsciScintilla(parent)
{
    setupEditor();
    resetEditor();
}

void PythonScintillaEditor::setupEditor()
{
    // Create Python lexer
    m_lexer = new QsciLexerPython(this);
    m_lexer->setDefaultPaper(QColor("#1e1e1e"));
    m_lexer->setDefaultColor(QColor("#d4d4d4"));
    m_lexer->setColor(QColor("#569cd6"), QsciLexerPython::Keyword);
    m_lexer->setColor(QColor("#dcdcaa"), QsciLexerPython::DoubleQuotedString);
    m_lexer->setColor(QColor("#dcdcaa"), QsciLexerPython::SingleQuotedString);
    m_lexer->setColor(QColor("#4ec9b0"), QsciLexerPython::ClassName);
    m_lexer->setColor(QColor("#4ec9b0"), QsciLexerPython::FunctionMethodName);
    m_lexer->setColor(QColor("#6a9955"), QsciLexerPython::Comment);
    setLexer(m_lexer);

    // Set up margins
    setMarginType(0, NumberMargin);
    setMarginWidth(0, "9999");  // Line numbers
    
    // Set encoding
    setUtf8(true);

    // Font configuration
    QFont font("Courier", 10);
    font.setStyleStrategy(QFont::PreferAntialias);
    setFont(font);

    // Text color configuration (similar to dark theme)
    setPaper(QColor("#1e1e1e"));
    setColor(QColor("#d4d4d4"));

    // Editor settings
    setWrapMode(WrapNone);
    setTabWidth(4);
    setIndentationsUseTabs(false);
    setAutoIndent(true);
    setBackspaceUnindents(true);
    setCaretLineVisible(true);
    setCaretLineBackgroundColor(QColor("#2d2d2d"));
    setCaretForegroundColor(QColor("#d4d4d4"));

    // Folding
    // setFolding is available but we'll keep simple for now
    // setFoldMarginColors(QColor("#3f3f3f"), QColor("#1e1e1e"));

    // Selection colors
    setSelectionForegroundColor(QColor("#d4d4d4"));
    setSelectionBackgroundColor(QColor("#264f78"));
}

void PythonScintillaEditor::showPrompt()
{
    int line, pos;
    getCursorPosition(&line, &pos);
    
    // Move to end of document
    line = lines() - 1;
    pos = lineLength(line);
    setCursorPosition(line, pos);

    // Add newline if needed so prompt is always on its own line.
    if (lineLength(line) > 0) {
        insert("\n");
        line = lines() - 1;
    }

    // Insert appropriate prompt
    if (m_awaitingContinuation) {
        insert(kContinuation);
    } else {
        insert(kPrompt);
    }

    m_promptLine = lines() - 1;

    // Position cursor at end of prompt
    line = lines() - 1;
    pos = lineLength(line);
    setCursorPosition(line, pos);

    ensureLineVisible(line);
}

void PythonScintillaEditor::appendOutput(const QString &text)
{
    int line = lines() - 1;
    int pos = lineLength(line);

    setCursorPosition(line, pos);

    // Always place output on a new line so it never becomes part of prompt input.
    if (lineLength(line) > 0) {
        insert("\n");
        line = lines() - 1;
        pos = 0;
        setCursorPosition(line, pos);
    }

    if (text.isEmpty()) {
        // Preserve existing behavior: empty output requests a blank spacer line.
        insert("\n");
        line = this->lines() - 1;
        pos = lineLength(line);
        setCursorPosition(line, pos);
        ensureLineVisible(line);
        return;
    }

    // Split text and add each line
    const QStringList outLines = text.split('\n', Qt::KeepEmptyParts);
    for (int i = 0; i < outLines.size(); ++i) {
        if (i > 0) {
            insert("\n");
        }
        insert(outLines[i]);
    }

    // Move to end
    line = this->lines() - 1;
    pos = lineLength(line);
    setCursorPosition(line, pos);
    ensureLineVisible(line);
}

QString PythonScintillaEditor::getCurrentCommand() const
{
    if (m_promptLine < 0 || m_promptLine >= lines()) {
        return QString();
    }

    QString line = this->text(m_promptLine);
    if (line.startsWith(kPrompt)) {
        return line.mid(4);
    } else if (line.startsWith(kContinuation)) {
        return line.mid(4);
    }
    return line;
}

void PythonScintillaEditor::resetEditor()
{
    clear();
    m_history.clear();
    m_historyIndex = -1;
    m_multilineBuffer.clear();
    m_awaitingContinuation = false;
    m_promptLine = -1;
    showPrompt();
}

void PythonScintillaEditor::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        handleEnterKey();
        return;
    }

    if (event->key() == Qt::Key_Backspace) {
        handleBackspace();
        return;
    }

    // History navigation
    if (event->key() == Qt::Key_Up && m_history.size() > 0) {
        navigateHistory(-1);
        return;
    } else if (event->key() == Qt::Key_Down && m_history.size() > 0) {
        navigateHistory(1);
        return;
    }

    // Readline bindings
    if (event->modifiers() & Qt::ControlModifier) {
        if (event->key() == Qt::Key_A) {
            moveCursorToInputStart();
            return;
        } else if (event->key() == Qt::Key_E) {
            moveCursorToInputEnd();
            return;
        } else if (event->key() == Qt::Key_K) {
            // Kill to end of line
            int line, pos;
            getCursorPosition(&line, &pos);
            int endPos = lineLength(line);
            if (endPos > pos) {
                setSelection(line, pos, line, endPos);
                removeSelectedText();
            }
            return;
        } else if (event->key() == Qt::Key_U) {
            // Kill entire line input
            int line, pos;
            getCursorPosition(&line, &pos);
            QString lineText = this->text(line);
            int promptLen = lineText.startsWith(kContinuation) ? 4 : 4;
            setSelection(line, promptLen, line, lineLength(line));
            removeSelectedText();
            return;
        }
    }

    // Tab handling
    if (event->key() == Qt::Key_Tab) {
        insert("    ");
        return;
    }

    // Paste handling
    if ((event->modifiers() & Qt::ControlModifier && event->key() == Qt::Key_V) ||
        (event->modifiers() & Qt::ShiftModifier && event->key() == Qt::Key_Insert)) {
        insert(QApplication::clipboard()->text());
        return;
    }

    QsciScintilla::keyPressEvent(event);
}

void PythonScintillaEditor::handleEnterKey()
{
    QString command = extractCurrentLineInput();

    if (command.isEmpty() && !m_awaitingContinuation) {
        showPrompt();
        return;
    }

    if (m_awaitingContinuation) {
        m_multilineBuffer += command + "\n";

        // Check if command is complete
        if (!command.isEmpty() && !command.endsWith(':') && !command.endsWith('\\')) {
            // Check bracket balance
            int openParens = m_multilineBuffer.count('(') - m_multilineBuffer.count(')');
            int openBrackets = m_multilineBuffer.count('[') - m_multilineBuffer.count(']');
            int openBraces = m_multilineBuffer.count('{') - m_multilineBuffer.count('}');

            if (openParens == 0 && openBrackets == 0 && openBraces == 0) {
                // Command is complete
                emit commandReady(m_multilineBuffer.trimmed());
                m_multilineBuffer.clear();
                m_awaitingContinuation = false;
                showPrompt();
                return;
            }
        }

        // Continue waiting
        showPrompt();
        return;
    }

    // Check if this starts a multi-line block
    if (isCompleteCommand(command)) {
        // Single-line complete command
        if (!m_history.isEmpty() && m_history.last() != command) {
            m_history.append(command);
        } else if (m_history.isEmpty()) {
            m_history.append(command);
        }
        m_historyIndex = -1;

        emit commandReady(command);
        showPrompt();
    } else {
        // Start multi-line mode
        m_multilineBuffer = command + "\n";
        m_awaitingContinuation = true;
        showPrompt();
    }
}

void PythonScintillaEditor::handleBackspace()
{
    int line, pos;
    getCursorPosition(&line, &pos);
    
    QString lineText = text(line);
    int promptLen = lineText.startsWith(kContinuation) ? 4 : 4;

    // Don't allow backspace past the prompt
    if (pos <= promptLen) {
        return;
    }

    QsciScintilla::keyPressEvent(new QKeyEvent(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier));
}

void PythonScintillaEditor::navigateHistory(int direction)
{
    if (m_history.isEmpty()) {
        return;
    }

    int newIndex = m_historyIndex + direction;
    if (newIndex < -1 || newIndex >= m_history.size()) {
        return;
    }

    m_historyIndex = newIndex;

    // Replace current line with history
    int line = lines() - 1;
    QString lineText = text(line);
    int promptLen = lineText.startsWith(kContinuation) ? 4 : 4;

    setSelection(line, promptLen, line, lineLength(line));
    if (m_historyIndex >= 0) {
        insert(m_history[m_historyIndex]);
    }
}

void PythonScintillaEditor::moveCursorToInputStart()
{
    int line, pos;
    getCursorPosition(&line, &pos);
    
    QString lineText = text(line);
    int promptLen = lineText.startsWith(kContinuation) ? 4 : 4;
    
    setCursorPosition(line, promptLen);
}

void PythonScintillaEditor::moveCursorToInputEnd()
{
    int line = lines() - 1;
    int pos = lineLength(line);
    setCursorPosition(line, pos);
}

QString PythonScintillaEditor::extractCurrentLineInput()
{
    int line = m_promptLine;
    if (line < 0 || line >= lines()) {
        line = lines() - 1;
    }
    QString lineText = text(line);

    if (lineText.startsWith(kContinuation)) {
        return lineText.mid(4);
    } else if (lineText.startsWith(kPrompt)) {
        return lineText.mid(4);
    }
    return lineText;
}

bool PythonScintillaEditor::isCompleteCommand(const QString &code)
{
    if (code.endsWith(':') || code.endsWith('\\')) {
        return false;
    }

    int openParens = code.count('(') - code.count(')');
    int openBrackets = code.count('[') - code.count(']');
    int openBraces = code.count('{') - code.count('}');

    return openParens == 0 && openBrackets == 0 && openBraces == 0;
}

void PythonScintillaEditor::insertPrompt(bool isContinuation)
{
    if (isContinuation) {
        insert(kContinuation);
    } else {
        insert(kPrompt);
    }
}

void PythonScintillaEditor::onMarginClicked(int margin, int /*line*/, Qt::KeyboardModifiers /*state*/)
{
    // Handle margin clicks for folding
    if (margin == 2) {  // Fold margin
        // Folding handled by QScintilla internally
    }
}
